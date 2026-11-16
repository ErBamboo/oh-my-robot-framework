#!/usr/bin/env python3
"""Boot layout source generator and checker.

The declarative layout file is the single source for:
- generated om_bootcfg.h / OM_BOOT_PARTITIONS
- generated boot_partition_data.c / boot_partition_data.h
- generated om_layout.json
- layout fingerprint

The tool deliberately reads TOML, not C headers. Host tools and linker scripts
must consume generated JSON or generated linker fragments, never parse C macros.

Usage:
    python tools/omlayout/omlayout.py generate --spec samples/boot/layout/rm-a-board.toml --out build/layout
    python tools/omlayout/omlayout.py check --spec samples/boot/layout/rm-a-board.toml \
        --linker-root platform/bsp/boards/rm-a-board/linker \
        --contract lib/boot/include/boot/image.h

Python 3.11+ is required for tomllib.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

SCHEMA_VERSION = 1
ROLE_BOOTLOADER = "bootloader"
ROLE_APP = "app"
ROLE_META = "meta"
ROLE_META_RESV = "meta_resv"
VALID_ROLES = {ROLE_BOOTLOADER, ROLE_APP, ROLE_META, ROLE_META_RESV}
VALID_SLOTS = {"a": 0, "b": 1}
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")
HEX_RE = re.compile(r"^(0x[0-9A-Fa-f]+|\d+)([KkMm]?)$")


class LayoutError(Exception):
    """The layout source is invalid or inconsistent with another artifact."""


@dataclass(frozen=True)
class EraseRegion:
    offset: int
    size: int
    count: int


@dataclass(frozen=True)
class Partition:
    name: str
    device: str
    offset: int
    size: int
    role: str
    slot: str | None
    slot_id: int | None
    link_base: int
    vtor_offset: int


@dataclass(frozen=True)
class ReservedRange:
    name: str
    offset: int
    size: int


@dataclass(frozen=True)
class Layout:
    schema: int
    board: str
    device: str
    flash_base: int
    flash_size: int
    erase_regions: tuple[EraseRegion, ...]
    payload_offset: int
    meta_copies: int
    rollback_limit: int
    partitions: tuple[Partition, ...]
    reserved: tuple[ReservedRange, ...]
    fingerprint: int
    layout_sha256: str


def _as_int(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise LayoutError(f"{label} must be an integer")
    if value < 0:
        raise LayoutError(f"{label} must not be negative")
    return value


def _as_str(value: object, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise LayoutError(f"{label} must be a non-empty string")
    if not NAME_RE.match(value):
        raise LayoutError(f"{label} contains unsupported characters: {value!r}")
    return value


def _required(data: dict, key: str, label: str) -> object:
    if key not in data:
        raise LayoutError(f"missing {label}.{key}")
    return data[key]


def _parse_erase_regions(raw: object, flash_size: int) -> tuple[EraseRegion, ...]:
    if not isinstance(raw, list) or not raw:
        raise LayoutError("flash.erase_regions must be a non-empty array")
    regions: list[EraseRegion] = []
    expected = 0
    for index, item in enumerate(raw):
        label = f"flash.erase_regions[{index}]"
        if not isinstance(item, dict):
            raise LayoutError(f"{label} must be a table")
        offset = _as_int(_required(item, "offset", label), f"{label}.offset")
        size = _as_int(_required(item, "size", label), f"{label}.size")
        count = _as_int(_required(item, "count", label), f"{label}.count")
        if size == 0 or count == 0:
            raise LayoutError(f"{label} size and count must be non-zero")
        end = offset + size * count
        if end > flash_size:
            raise LayoutError(f"{label} exceeds flash size")
        if offset != expected:
            raise LayoutError(
                f"{label} starts at 0x{offset:X}, expected 0x{expected:X}; "
                "erase regions must be contiguous"
            )
        regions.append(EraseRegion(offset=offset, size=size, count=count))
        expected = end
    if expected != flash_size:
        raise LayoutError(
            f"erase regions cover 0x{expected:X} bytes, flash.size is 0x{flash_size:X}"
        )
    return tuple(regions)


def _erase_units_within(
    offset: int, size: int, regions: tuple[EraseRegion, ...]
) -> list[int] | None:
    """Return erase-unit sizes covering [offset, offset+size), or None."""
    end = offset + size
    if size == 0 or offset + size < offset:
        return None
    units: list[int] = []
    cur = offset
    for region in regions:
        region_start = region.offset
        region_end = region_start + region.size * region.count
        if cur >= region_end:
            continue
        if cur < region_start:
            return None
        relative = cur - region_start
        if relative % region.size:
            return None
        available = region_end - cur
        take = min(end - cur, available)
        if take % region.size:
            return None
        units.extend([region.size] * (take // region.size))
        cur += take
        if cur == end:
            return units
    return None


def _check_non_overlap(ranges: list[tuple[int, int, str]]) -> None:
    ordered = sorted(ranges, key=lambda item: (item[0], item[1], item[2]))
    previous_end = -1
    previous_name = ""
    for offset, size, name in ordered:
        if size <= 0:
            raise LayoutError(f"range {name} must have positive size")
        if offset < previous_end:
            raise LayoutError(
                f"range {name} at 0x{offset:X} overlaps {previous_name} "
                f"(previous end 0x{previous_end:X})"
            )
        previous_end = offset + size
        previous_name = name


def parse_layout(data: dict, source: str = "<layout>") -> Layout:
    """Validate a parsed TOML object and return the normalized layout."""
    schema = _as_int(_required(data, "schema", source), f"{source}.schema")
    if schema != SCHEMA_VERSION:
        raise LayoutError(f"unsupported schema {schema}, expected {SCHEMA_VERSION}")

    board = _as_str(_required(data, "board", source), f"{source}.board")
    device = _as_str(_required(data, "device", source), f"{source}.device")
    payload_offset = _as_int(
        _required(data, "payload_offset", source), f"{source}.payload_offset"
    )
    meta_copies = _as_int(
        _required(data, "meta_copies", source), f"{source}.meta_copies"
    )
    rollback_limit = _as_int(
        _required(data, "rollback_limit", source), f"{source}.rollback_limit"
    )
    if payload_offset == 0 or payload_offset % 0x200:
        raise LayoutError("payload_offset must be non-zero and 0x200 aligned")
    if meta_copies != 2:
        raise LayoutError("meta_copies must be 2 for the current boot design")
    if rollback_limit == 0:
        raise LayoutError("rollback_limit must be non-zero")

    flash = _required(data, "flash", source)
    if not isinstance(flash, dict):
        raise LayoutError(f"{source}.flash must be a table")
    flash_base = _as_int(_required(flash, "base", f"{source}.flash"), "flash.base")
    flash_size = _as_int(_required(flash, "size", f"{source}.flash"), "flash.size")
    if flash_size == 0:
        raise LayoutError("flash.size must be non-zero")
    if flash_base + flash_size > 0x1_0000_0000:
        raise LayoutError("flash range exceeds 32-bit address space")
    erase_regions = _parse_erase_regions(
        _required(flash, "erase_regions", f"{source}.flash"), flash_size
    )

    raw_partitions = _required(data, "partitions", source)
    if not isinstance(raw_partitions, list) or not raw_partitions:
        raise LayoutError(f"{source}.partitions must be a non-empty array")

    partitions: list[Partition] = []
    names: set[str] = set()
    for index, item in enumerate(raw_partitions):
        label = f"{source}.partitions[{index}]"
        if not isinstance(item, dict):
            raise LayoutError(f"{label} must be a table")
        name = _as_str(_required(item, "name", label), f"{label}.name")
        if name in names:
            raise LayoutError(f"duplicate partition name: {name}")
        names.add(name)
        offset = _as_int(_required(item, "offset", label), f"{label}.offset")
        size = _as_int(_required(item, "size", label), f"{label}.size")
        role = _as_str(_required(item, "role", label), f"{label}.role")
        if role not in VALID_ROLES:
            raise LayoutError(f"{label}.role is unknown: {role}")
        if size == 0:
            raise LayoutError(f"{label}.size must be non-zero")
        if offset > flash_size or size > flash_size - offset:
            raise LayoutError(f"{label} exceeds flash size")
        if _erase_units_within(offset, size, erase_regions) is None:
            raise LayoutError(f"{label} is not aligned to erase units")
        part_device = item.get("device", device)
        part_device = _as_str(part_device, f"{label}.device")

        slot: str | None = None
        slot_id: int | None = None
        if role == ROLE_APP:
            slot = _as_str(_required(item, "slot", label), f"{label}.slot")
            if slot not in VALID_SLOTS:
                raise LayoutError(f"{label}.slot must be 'a' or 'b'")
            expected_id = VALID_SLOTS[slot]
            if "slot_id" in item:
                slot_id = _as_int(item["slot_id"], f"{label}.slot_id")
                if slot_id != expected_id:
                    raise LayoutError(
                        f"{label}.slot_id {slot_id} does not match slot {slot}"
                    )
            else:
                slot_id = expected_id
            if offset % 0x200:
                raise LayoutError(f"{label}.offset must be 0x200 aligned")
            if payload_offset >= size:
                raise LayoutError(f"{label}.size is too small for payload_offset")
            link_base = flash_base + offset + payload_offset
            vtor_offset = offset + payload_offset
        else:
            if "slot" in item or "slot_id" in item:
                raise LayoutError(f"{label}.slot is only valid for role app")
            link_base = flash_base + offset
            vtor_offset = 0

        partitions.append(
            Partition(
                name=name,
                device=part_device,
                offset=offset,
                size=size,
                role=role,
                slot=slot,
                slot_id=slot_id,
                link_base=link_base,
                vtor_offset=vtor_offset,
            )
        )

    apps = [p for p in partitions if p.role == ROLE_APP]
    bootloaders = [p for p in partitions if p.role == ROLE_BOOTLOADER]
    metas = [p for p in partitions if p.role == ROLE_META]
    meta_resv = [p for p in partitions if p.role == ROLE_META_RESV]
    if len(bootloaders) != 1:
        raise LayoutError("layout must contain exactly one bootloader partition")
    if len(metas) != 1:
        raise LayoutError("layout must contain exactly one meta partition")
    if len(meta_resv) > 1:
        raise LayoutError("layout must contain at most one meta_resv partition")
    if {p.slot for p in apps} != {"a", "b"}:
        raise LayoutError("layout must contain app partitions for slots a and b")
    app_sizes = {p.size for p in apps}
    if len(app_sizes) != 1:
        raise LayoutError("app_a and app_b must have the same size for direct A/B")

    meta_units = _erase_units_within(metas[0].offset, metas[0].size, erase_regions)
    if meta_units is None or len(meta_units) < meta_copies:
        raise LayoutError(
            f"meta partition has {0 if meta_units is None else len(meta_units)} "
            f"erase units, needs at least {meta_copies}"
        )

    raw_reserved = data.get("reserved", [])
    if not isinstance(raw_reserved, list):
        raise LayoutError(f"{source}.reserved must be an array")
    reserved: list[ReservedRange] = []
    for index, item in enumerate(raw_reserved):
        label = f"{source}.reserved[{index}]"
        if not isinstance(item, dict):
            raise LayoutError(f"{label} must be a table")
        r_offset = _as_int(_required(item, "offset", label), f"{label}.offset")
        r_size = _as_int(_required(item, "size", label), f"{label}.size")
        r_name = _as_str(item.get("name", f"reserved{index}"), f"{label}.name")
        if r_offset > flash_size or r_size > flash_size - r_offset:
            raise LayoutError(f"{label} exceeds flash size")
        if _erase_units_within(r_offset, r_size, erase_regions) is None:
            raise LayoutError(f"{label} is not aligned to erase units")
        reserved.append(ReservedRange(name=r_name, offset=r_offset, size=r_size))

    ranges: list[tuple[int, int, str]] = []
    for p in partitions:
        ranges.append((p.offset, p.size, p.name))
    for r in reserved:
        ranges.append((r.offset, r.size, r.name))
    _check_non_overlap(ranges)

    source_manifest = {
        "schema": schema,
        "board": board,
        "device": device,
        "flash": {
            "base": flash_base,
            "size": flash_size,
            "erase_regions": [
                {"offset": r.offset, "size": r.size, "count": r.count}
                for r in erase_regions
            ],
        },
        "payload_offset": payload_offset,
        "meta_copies": meta_copies,
        "rollback_limit": rollback_limit,
        "partitions": [
            {
                "name": p.name,
                "device": p.device,
                "offset": p.offset,
                "size": p.size,
                "role": p.role,
                "slot": p.slot,
                "slot_id": p.slot_id,
            }
            for p in sorted(partitions, key=lambda item: item.offset)
        ],
        "reserved": [
            {"name": r.name, "offset": r.offset, "size": r.size}
            for r in sorted(reserved, key=lambda item: item.offset)
        ],
    }
    canonical = json.dumps(
        source_manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    layout_sha256 = hashlib.sha256(canonical).hexdigest()
    fingerprint = int(layout_sha256[:4], 16)

    return Layout(
        schema=schema,
        board=board,
        device=device,
        flash_base=flash_base,
        flash_size=flash_size,
        erase_regions=erase_regions,
        payload_offset=payload_offset,
        meta_copies=meta_copies,
        rollback_limit=rollback_limit,
        partitions=tuple(sorted(partitions, key=lambda item: item.offset)),
        reserved=tuple(sorted(reserved, key=lambda item: item.offset)),
        fingerprint=fingerprint,
        layout_sha256=layout_sha256,
    )


def load_layout(path: Path) -> Layout:
    try:
        with path.open("rb") as stream:
            data = tomllib.load(stream)
    except OSError as exc:
        raise LayoutError(f"cannot read {path}: {exc}") from exc
    except tomllib.TOMLDecodeError as exc:
        raise LayoutError(f"invalid TOML in {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise LayoutError(f"{path}: top-level value must be a table")
    return parse_layout(data, str(path))


def _hex(value: int) -> str:
    return f"0x{value:08X}u"


def _manifest(layout: Layout) -> dict:
    slots = {}
    for part in layout.partitions:
        if part.role != ROLE_APP:
            continue
        slots[part.slot] = {
            "partition": part.name,
            "slot_id": part.slot_id,
            "offset": part.offset,
            "size": part.size,
            "link_base": part.link_base,
            "vtor_offset": part.vtor_offset,
        }
    meta = next(p for p in layout.partitions if p.role == ROLE_META)
    boot = next(p for p in layout.partitions if p.role == ROLE_BOOTLOADER)
    return {
        "schema": layout.schema,
        "board": layout.board,
        "device": layout.device,
        "flash": {
            "base": layout.flash_base,
            "size": layout.flash_size,
            "erase_regions": [
                {"offset": r.offset, "size": r.size, "count": r.count}
                for r in layout.erase_regions
            ],
        },
        "payload_offset": layout.payload_offset,
        "meta_copies": layout.meta_copies,
        "rollback_limit": layout.rollback_limit,
        "fingerprint": layout.fingerprint,
        "layout_sha256": layout.layout_sha256,
        "partitions": [
            {
                "name": p.name,
                "device": p.device,
                "offset": p.offset,
                "size": p.size,
                "role": p.role,
                "slot": p.slot,
                "slot_id": p.slot_id,
                "link_base": p.link_base,
                "vtor_offset": p.vtor_offset,
            }
            for p in layout.partitions
        ],
        "slots": slots,
        "bootloader": {
            "partition": boot.name,
            "offset": boot.offset,
            "size": boot.size,
        },
        "meta": {
            "partition": meta.name,
            "offset": meta.offset,
            "size": meta.size,
            "copies": layout.meta_copies,
            "erase_unit_count": len(
                _erase_units_within(meta.offset, meta.size, layout.erase_regions)
                or []
            ),
        },
        "reserved": [
            {"name": r.name, "offset": r.offset, "size": r.size}
            for r in layout.reserved
        ],
    }


def render_bootcfg_header(layout: Layout) -> str:
    lines = [
        "/*",
        " * Generated by tools/omlayout/omlayout.py.",
        " * Do not edit. Change the layout source and regenerate.",
        " */",
        "#ifndef OM_BOOTCFG_H",
        "#define OM_BOOTCFG_H",
        "",
        "#define OM_BOOT_PARTITIONS \\",
    ]
    for index, part in enumerate(layout.partitions):
        suffix = " \\" if index + 1 < len(layout.partitions) else ""
        lines.append(
            f'    X("{part.name}", "{part.device}", {_hex(part.offset)}, '
            f"{_hex(part.size)}){suffix}"
        )
    lines.extend(
        [
            "",
            f"#define OM_IMAGE_PAYLOAD_OFFSET {_hex(layout.payload_offset)}",
            f"#define OM_BOOT_META_COPIES {layout.meta_copies}u",
            f"#define OM_BOOT_ROLLBACK_LIMIT {layout.rollback_limit}u",
            f"#define OM_BOOT_LAYOUT_FINGERPRINT {_hex(layout.fingerprint)}",
            f"#define OM_BOOT_PARTITION_COUNT {len(layout.partitions)}u",
            f"#define OM_BOOT_FLASH_BASE {_hex(layout.flash_base)}",
            f"#define OM_BOOT_FLASH_SIZE {_hex(layout.flash_size)}",
            "",
        ]
    )
    boot = next(p for p in layout.partitions if p.role == ROLE_BOOTLOADER)
    meta = next(p for p in layout.partitions if p.role == ROLE_META)
    lines.append(f"#define OM_BOOT_BOOTLOADER_OFFSET {_hex(boot.offset)}")
    lines.append(f"#define OM_BOOT_BOOTLOADER_SIZE {_hex(boot.size)}")
    lines.append(f"#define OM_BOOT_META_OFFSET {_hex(meta.offset)}")
    lines.append(f"#define OM_BOOT_META_SIZE {_hex(meta.size)}")
    for part in sorted(
        (p for p in layout.partitions if p.role == ROLE_APP), key=lambda p: p.slot
    ):
        prefix = f"OM_BOOT_SLOT_{part.slot.upper()}"
        lines.append(f"#define {prefix}_BASE {_hex(part.offset)}")
        lines.append(f"#define {prefix}_SIZE {_hex(part.size)}")
        lines.append(f"#define {prefix}_LINK_BASE {_hex(part.link_base)}")
        lines.append(f"#define {prefix}_VTOR_OFFSET {_hex(part.vtor_offset)}")
    lines.extend(["", "#endif /* OM_BOOTCFG_H */", ""])
    return "\n".join(lines)


def render_partition_data_c(layout: Layout) -> str:
    return (
        "/*\n"
        " * Generated by tools/omlayout/omlayout.py.\n"
        " * Do not edit. Change the layout source and regenerate.\n"
        " */\n"
        '#include "drivers/storage/partition.h"\n'
        '#include "om_bootcfg.h"\n'
        "\n"
        "#define X(name_, device_, offset_, size_) {name_, device_, offset_, size_},\n"
        "static const OmPartitionEntry s_boot_partitions[] = {\n"
        "    OM_BOOT_PARTITIONS\n"
        "};\n"
        "#undef X\n"
        "\n"
        "static const OmPartitionRegistry s_boot_registry =\n"
        "    OM_PARTITION_REGISTRY(s_boot_partitions);\n"
        "\n"
        "const OmPartitionRegistry *om_boot_partition_registry(void)\n"
        "{\n"
        "    return &s_boot_registry;\n"
        "}\n"
    )


def render_partition_data_h() -> str:
    return (
        "/*\n"
        " * Generated by tools/omlayout/omlayout.py.\n"
        " * Do not edit. Change the layout source and regenerate.\n"
        " */\n"
        "#ifndef OM_BOOT_PARTITION_DATA_H\n"
        "#define OM_BOOT_PARTITION_DATA_H\n"
        "\n"
        '#include "drivers/storage/partition.h"\n'
        "\n"
        "const OmPartitionRegistry *om_boot_partition_registry(void);\n"
        "\n"
        "#endif /* OM_BOOT_PARTITION_DATA_H */\n"
    )


def write_outputs(layout: Layout, out_dir: Path) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    outputs = {
        "om_bootcfg.h": render_bootcfg_header(layout),
        "boot_partition_data.c": render_partition_data_c(layout),
        "boot_partition_data.h": render_partition_data_h(),
        "om_layout.json": json.dumps(
            _manifest(layout), indent=2, sort_keys=True, ensure_ascii=False
        )
        + "\n",
    }
    written: list[Path] = []
    for name, content in outputs.items():
        path = out_dir / name
        path.write_text(content, encoding="utf-8", newline="\n")
        written.append(path)
    return written


def _parse_number(text: str, label: str) -> int:
    match = HEX_RE.match(text.strip())
    if not match:
        raise LayoutError(f"cannot parse {label}: {text!r}")
    value = int(match.group(1), 0)
    suffix = match.group(2).lower()
    if suffix == "k":
        value *= 1024
    elif suffix == "m":
        value *= 1024 * 1024
    return value


def _gcc_region(text: str, path: Path) -> tuple[int, int]:
    match = re.search(
        r"\bROM\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*"
        r"(0x[0-9A-Fa-f]+|\d+[KkMm]?)\s*,\s*LENGTH\s*=\s*"
        r"(0x[0-9A-Fa-f]+|\d+[KkMm]?)",
        text,
    )
    if not match:
        raise LayoutError(f"{path}: cannot find ROM ORIGIN/LENGTH")
    return _parse_number(match.group(1), "ROM origin"), _parse_number(
        match.group(2), "ROM length"
    )


def _arm_region(text: str, path: Path) -> tuple[int, int]:
    match = re.search(
        r"\bLR_IROM1\s+(0x[0-9A-Fa-f]+|\d+[KkMm]?)\s+"
        r"(0x[0-9A-Fa-f]+|\d+[KkMm]?)",
        text,
    )
    if not match:
        raise LayoutError(f"{path}: cannot find LR_IROM1 origin/length")
    return _parse_number(match.group(1), "LR_IROM1 origin"), _parse_number(
        match.group(2), "LR_IROM1 length"
    )


def check_linkers(layout: Layout, linker_root: Path) -> list[str]:
    boot = next(p for p in layout.partitions if p.role == ROLE_BOOTLOADER)
    apps = {p.slot: p for p in layout.partitions if p.role == ROLE_APP}
    expected = {
        "bootloader": (layout.flash_base + boot.offset, boot.size),
        "app_a": (
            apps["a"].link_base,
            apps["a"].size - layout.payload_offset,
        ),
        "app_b": (
            apps["b"].link_base,
            apps["b"].size - layout.payload_offset,
        ),
    }
    checks: list[tuple[Path, str, int, int]] = []
    for name in ("bootloader", "app_a", "app_b"):
        gcc_path = linker_root / "gcc" / f"{name}.ld"
        arm_path = linker_root / "arm" / f"{name}.sct"
        checks.append((gcc_path, "gcc", expected[name][0], expected[name][1]))
        checks.append((arm_path, "arm", expected[name][0], expected[name][1]))

    messages: list[str] = []
    for path, toolchain, want_origin, want_length in checks:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            raise LayoutError(f"cannot read linker script {path}: {exc}") from exc
        if toolchain == "gcc":
            origin, length = _gcc_region(text, path)
        else:
            origin, length = _arm_region(text, path)
        if origin != want_origin or length != want_length:
            raise LayoutError(
                f"{path}: linker region is 0x{origin:08X}/0x{length:X}, "
                f"layout expects 0x{want_origin:08X}/0x{want_length:X}"
            )
        messages.append(f"{path}: 0x{origin:08X}/0x{length:X} OK")
    return messages


def check_contract(layout: Layout, contract_path: Path) -> str:
    try:
        text = contract_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise LayoutError(f"cannot read contract {contract_path}: {exc}") from exc
    match = re.search(
        r"#define\s+OM_IMAGE_PAYLOAD_OFFSET\s+(0x[0-9A-Fa-f]+|\d+)[uU]?", text
    )
    if not match:
        raise LayoutError(f"{contract_path}: cannot find OM_IMAGE_PAYLOAD_OFFSET")
    value = int(match.group(1), 0)
    if value != layout.payload_offset:
        raise LayoutError(
            f"{contract_path}: payload offset 0x{value:X} != layout "
            f"0x{layout.payload_offset:X}"
        )
    return f"{contract_path}: payload offset 0x{value:X} OK"


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")

    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate", help="generate C header/table and JSON")
    gen.add_argument("--spec", required=True, type=Path)
    gen.add_argument("--out", required=True, type=Path)
    check = sub.add_parser("check", help="validate a layout source and consumers")
    check.add_argument("--spec", required=True, type=Path)
    check.add_argument("--linker-root", type=Path)
    check.add_argument("--contract", type=Path)
    args = parser.parse_args(argv)

    try:
        layout = load_layout(args.spec)
        if args.command == "generate":
            paths = write_outputs(layout, args.out)
            for path in paths:
                print(path)
            print(f"layout OK: {layout.board}, fingerprint 0x{layout.fingerprint:04X}")
            return 0

        messages = [f"layout OK: {layout.board}, fingerprint 0x{layout.fingerprint:04X}"]
        if args.linker_root:
            messages.extend(check_linkers(layout, args.linker_root))
        if args.contract:
            messages.append(check_contract(layout, args.contract))
        for message in messages:
            print(message)
        return 0
    except LayoutError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
