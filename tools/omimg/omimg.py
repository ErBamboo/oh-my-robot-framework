#!/usr/bin/env python3
"""镜像打包与检查——镜像格式契约的主机侧实现。

契约的事实源是 C 头文件 lib/boot/include/boot/image.h；本文件的常量与之镜像，
字节布局必须逐字段一致。`selftest` 会解析该头文件核对常量，避免两侧漂移。

用法：
    python omimg.py pack --input app.bin --output app_a.bin --version 1.2.3 --slot a
    python omimg.py inspect app_a.bin
    python omimg.py verify app_a.bin [--slot-capacity 0xF0000]
    python omimg.py selftest
"""

import argparse
import hashlib
import re
import struct
import sys
import zlib
from pathlib import Path

# ---------------------------------------------------------------------------
# 契约常量（镜像自 lib/boot/include/boot/image.h）
# ---------------------------------------------------------------------------

MAGIC = 0x4F4D5249  # "OMRI"
HDR_VERSION = 1
HDR_SIZE = 64
PAYLOAD_OFFSET = 0x200
DIGEST_REGION_SIZE = 256
FILL = 0xFF

DIGEST_NONE = 0
DIGEST_CRC32_ISO_HDLC = 1
DIGEST_SHA256 = 2
DIGEST_LEN = {DIGEST_NONE: 0, DIGEST_CRC32_ISO_HDLC: 4, DIGEST_SHA256: 32}

F_NON_BOOTABLE = 0x00000001
F_SLOT_BOUND = 0x00000100
F_DIGEST_IN_META = 0x00000200
F_VENDOR_BASE = 0x00010000

SLOT_ID = {"a": 0, "b": 1}

FLAG_NAMES = {
    F_NON_BOOTABLE: "NON_BOOTABLE",
    F_SLOT_BOUND: "SLOT_BOUND",
    F_DIGEST_IN_META: "DIGEST_IN_META",
}

ALGO_NAMES = {
    DIGEST_NONE: "NONE",
    DIGEST_CRC32_ISO_HDLC: "CRC32_ISO_HDLC",
    DIGEST_SHA256: "SHA256",
}

# 兜底值＝上面的字面量。正常运行时不使用它们：main 会先从 C 契约头加载真值
# （契约头是唯一事实源），兜底只在契约头不可读时生效，并由自检报告其是否陈旧。
_FALLBACK = {
    "MAGIC": MAGIC,
    "HDR_VERSION": HDR_VERSION,
    "HDR_SIZE": HDR_SIZE,
    "PAYLOAD_OFFSET": PAYLOAD_OFFSET,
    "DIGEST_REGION_SIZE": DIGEST_REGION_SIZE,
}

# 跨语言对拍基准（与 host 语料同源）：同一负载与元数据在两侧必须得到同一摘要
FIXTURE = {
    "payload_len": 1300,
    "version": 0x01020300,
    "slot": 0,
    "flags": F_SLOT_BOUND,
    "image_size": 1300,
    "total_size": 2068,
    "digest_offset": 0x714,
    "digest": 0x947D81DC,
}

# 头字段：15 项，全小端、无对齐填充
_HDR_FMT = "<IHHIIIIIHHIHHI20s"
_HDR_FIELDS = (
    "magic",
    "hdrVersion",
    "hdrSize",
    "payloadOffset",
    "imageSize",
    "imageTotalSize",
    "imageVersion",
    "flags",
    "digestAlgo",
    "digestLen",
    "digestOffset",
    "digestRegionSize",
    "slotId",
    "headerCrc32",
    "reserved",
)

_HEADER_PATH = Path(__file__).resolve().parents[2] / "lib" / "boot" / "include" / "boot" / "image.h"


class ImageError(Exception):
    """镜像不合法（判定顺序中任一步拒绝）。"""


# ---------------------------------------------------------------------------
# 摘要
# ---------------------------------------------------------------------------


def compute_digest(algo, data):
    """按契约算法计算摘要，返回写入摘要区的字节串。"""
    if algo == DIGEST_NONE:
        return b""
    if algo == DIGEST_CRC32_ISO_HDLC:
        # CRC-32/ISO-HDLC 的值按小端写入（与格式其余部分一致）
        return struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)
    if algo == DIGEST_SHA256:
        return hashlib.sha256(data).digest()
    raise ImageError("未知摘要算法：%r" % (algo,))


# ---------------------------------------------------------------------------
# 打包
# ---------------------------------------------------------------------------


def build_image(
    payload,
    *,
    version=0,
    slot=SLOT_ID["a"],
    algo=DIGEST_CRC32_ISO_HDLC,
    flags=None,
    payload_offset=None,
    digest_region_size=None,
):
    """由裸负载构造完整镜像（头 + 填充 + 负载 + 摘要区）。

    payload_offset / digest_region_size 缺省取当前生效的契约值（load_contract 后
    即契约头中的值）。
    """
    if payload_offset is None:
        payload_offset = PAYLOAD_OFFSET
    if digest_region_size is None:
        digest_region_size = DIGEST_REGION_SIZE
    if payload_offset < HDR_SIZE or payload_offset % 4:
        raise ImageError("payload_offset 必须 >= 头长且 4 字节对齐")
    if payload_offset % 0x200:
        raise ImageError("payload_offset 必须 0x200 对齐（向量表约束）")
    if digest_region_size < DIGEST_LEN.get(algo, 0):
        raise ImageError("摘要区预留小于算法所需长度")
    if flags is None:
        flags = F_SLOT_BOUND

    # 负载补齐到 4 字节倍数：摘要覆盖区长度须为 4 的倍数
    pad = (-len(payload)) % 4
    payload_padded = payload + bytes([FILL]) * pad
    image_size = len(payload_padded)

    digest_offset = payload_offset + image_size
    image_total_size = digest_offset + digest_region_size

    header = struct.pack(
        _HDR_FMT,
        MAGIC,
        HDR_VERSION,
        HDR_SIZE,
        payload_offset,
        image_size,
        image_total_size,
        version & 0xFFFFFFFF,
        flags & 0xFFFFFFFF,
        algo,
        DIGEST_LEN[algo],
        digest_offset,
        digest_region_size,
        slot,
        0,  # headerCrc32：未启用
        bytes([FILL]) * 20,
    )
    if len(header) != HDR_SIZE:
        raise ImageError(
            "头布局串算出的长度 %d 与契约头的头长 %d 不符——字段增删后本文件的 _HDR_FMT "
            "必须同步更新，且契约头长须走版本演进" % (len(header), HDR_SIZE)
        )

    body = header + bytes([FILL]) * (payload_offset - HDR_SIZE) + payload_padded
    digest = compute_digest(algo, body)
    region = digest + bytes([FILL]) * (digest_region_size - len(digest))
    return body + region


def parse_image(data):
    """解析头部为字典；仅做长度检查，不做合法性判定。"""
    if len(data) < HDR_SIZE:
        raise ImageError("数据不足一个头（%d < %d）" % (len(data), HDR_SIZE))
    values = struct.unpack(_HDR_FMT, data[:HDR_SIZE])
    return dict(zip(_HDR_FIELDS, values))


def verify_image(data, slot_capacity=None):
    """按契约判定顺序校验镜像，返回（是否通过, 说明列表）。"""
    notes = []
    try:
        h = parse_image(data)
    except ImageError as exc:
        return False, [str(exc)]

    if h["magic"] != MAGIC:
        return False, ["magic 不符（0x%08X）——空白槽或非本框架镜像" % h["magic"]]
    if h["hdrVersion"] != HDR_VERSION:
        return False, ["头部版本不符（%d != %d）" % (h["hdrVersion"], HDR_VERSION)]
    if h["hdrSize"] != HDR_SIZE:
        return False, ["头长字段不符（%d != %d）" % (h["hdrSize"], HDR_SIZE)]
    if h["flags"] & F_NON_BOOTABLE:
        return False, ["镜像标记为不可启动（NON_BOOTABLE）"]

    # 尺寸自洽（先于任何基于头内尺寸的访问）
    payload_offset = h["payloadOffset"]
    image_size = h["imageSize"]
    if payload_offset < HDR_SIZE or payload_offset % 0x200:
        return False, ["负载偏移不合法：0x%X" % payload_offset]
    if image_size % 4:
        return False, ["负载长度非 4 的倍数：%d" % image_size]
    if h["imageTotalSize"] != payload_offset + image_size + h["digestRegionSize"]:
        return False, ["总长字段不自洽"]
    if h["digestOffset"] != payload_offset + image_size:
        return False, ["摘要偏移不自洽"]
    # 未知算法在此按"无需预留"处理，留给下面的算法校验步骤拒绝
    if h["digestRegionSize"] < DIGEST_LEN.get(h["digestAlgo"], 0):
        return False, ["摘要区预留不足"]

    total = h["imageTotalSize"]
    if slot_capacity is not None and total > slot_capacity:
        return False, ["镜像总长 0x%X 超出槽容量 0x%X" % (total, slot_capacity)]
    if len(data) < total:
        return False, ["数据不足：需要 0x%X，实有 0x%X" % (total, len(data))]

    algo = h["digestAlgo"]
    if algo not in DIGEST_LEN:
        return False, ["未知摘要算法 ID：%d（不得用未知算法尝试校验）" % algo]
    if h["digestLen"] != DIGEST_LEN[algo]:
        return False, ["摘要长度与算法不符（%d != %d）" % (h["digestLen"], DIGEST_LEN[algo])]

    covered = h["digestOffset"]
    expect = compute_digest(algo, data[:covered])
    actual = data[h["digestOffset"] : h["digestOffset"] + h["digestLen"]]
    if expect != actual:
        return False, ["摘要不符：期望 %s，实得 %s" % (expect.hex(), actual.hex())]

    notes.append("镜像合法：负载 %d 字节，摘要 %s" % (image_size, ALGO_NAMES[algo]))
    return True, notes


def parse_version(text):
    """'1.2.3' → major<<24 | minor<<16 | patch<<8"""
    parts = [int(p) for p in re.split(r"[._-]", text.strip())]
    while len(parts) < 3:
        parts.append(0)
    major, minor, patch = parts[0], parts[1], parts[2]
    for v, name in ((major, "major"), (minor, "minor"), (patch, "patch")):
        if not 0 <= v <= 0xFF:
            raise ImageError("版本段 %s 超出 0..255：%d" % (name, v))
    return (major << 24) | (minor << 16) | (patch << 8)


# ---------------------------------------------------------------------------
# 自检
# ---------------------------------------------------------------------------


def _read_header_constants():
    """从 C 契约头解析契约常量（防两侧漂移）。"""
    try:
        text = _HEADER_PATH.read_text(encoding="utf-8")
    except OSError as exc:
        raise ImageError("无法读取契约头 %s：%s" % (_HEADER_PATH, exc))

    def num(name):
        m = re.search(r"#define\s+%s\s+([0-9xXa-fA-FuUlL]+)" % name, text)
        if not m:
            raise ImageError("契约头中找不到 %s" % name)
        return int(re.sub(r"[uUlL]+$", "", m.group(1)), 0)

    return {
        "MAGIC": num("OM_IMAGE_MAGIC"),
        "HDR_VERSION": num("OM_IMAGE_HDR_VERSION"),
        "HDR_SIZE": num("OM_IMAGE_HDR_SIZE"),
        "PAYLOAD_OFFSET": num("OM_IMAGE_PAYLOAD_OFFSET"),
        "DIGEST_REGION_SIZE": num("OM_IMAGE_DIGEST_REGION_SIZE"),
    }


def load_contract():
    """从 C 契约头加载常量并覆盖本模块的字面量（契约头 = 唯一事实源）。

    契约头不可读时保留兜底值并给出警告——工具仍可用，但自检会报告失败。
    返回（是否加载成功, 说明）。
    """
    global MAGIC, HDR_VERSION, HDR_SIZE, PAYLOAD_OFFSET, DIGEST_REGION_SIZE
    try:
        values = _read_header_constants()
    except ImageError as exc:
        return False, "未能从契约头加载常量，使用内置兜底值：%s" % exc

    MAGIC = values["MAGIC"]
    HDR_VERSION = values["HDR_VERSION"]
    HDR_SIZE = values["HDR_SIZE"]
    PAYLOAD_OFFSET = values["PAYLOAD_OFFSET"]
    DIGEST_REGION_SIZE = values["DIGEST_REGION_SIZE"]
    return True, ""


def selftest():
    """返回（是否通过, 说明列表）。覆盖：算法自检向量、契约常量一致、往返与损坏检测。"""
    ok = True
    notes = []

    # 1) canonical 算法自检向量
    got = zlib.crc32(b"123456789") & 0xFFFFFFFF
    if got != 0xCBF43926:
        ok = False
        notes.append("FAIL CRC-32/ISO-HDLC 自检向量：0x%08X" % got)
    else:
        notes.append("OK   CRC-32/ISO-HDLC 自检向量 0xCBF43926")

    # 2) 契约头可读，且内置兜底不陈旧（兜底只在契约头不可读时生效，陈旧即隐性漂移）
    try:
        header = _read_header_constants()
    except ImageError as exc:
        ok = False
        notes.append("FAIL 契约头不可读：%s" % exc)
    else:
        stale = [name for name, value in _FALLBACK.items() if header[name] != value]
        if stale:
            ok = False
            notes.append("FAIL 内置兜底值陈旧（与契约头不符）：%s" % ", ".join(stale))
        else:
            notes.append("OK   契约头可读且内置兜底一致（%s）" % _HEADER_PATH.name)

    # 3) 往返：打包 → 校验通过；改一个负载字节 → 被拒绝
    payload = bytes(range(256)) * 5 + b"\x01\x02\x03"  # 非 4 倍数，触发补齐
    image = build_image(payload, version=parse_version("1.2.3"), slot=SLOT_ID["b"])
    good, detail = verify_image(image, slot_capacity=0xF0000)
    if not good:
        ok = False
        notes.append("FAIL 往返校验未通过：%s" % detail)
    else:
        notes.append("OK   往返校验（%d 字节负载 → %d 字节镜像）" % (len(payload), len(image)))

    corrupted = bytearray(image)
    corrupted[PAYLOAD_OFFSET + 8] ^= 0x01
    rejected, _ = verify_image(bytes(corrupted), slot_capacity=0xF0000)
    if rejected:
        ok = False
        notes.append("FAIL 负载损坏未被检出")
    else:
        notes.append("OK   负载损坏被检出")

    # 4) 边界：空负载
    empty = build_image(b"", version=1)
    good, detail = verify_image(empty)
    if not good:
        ok = False
        notes.append("FAIL 空负载镜像未通过：%s" % detail)
    else:
        notes.append("OK   空负载镜像通过校验")

    # 5) 跨语言对拍基准：与 host 语料断言同一组值（任一侧布局漂移都会被抓住）
    payload = bytes(((i * 37 + 11) & 0xFF) for i in range(FIXTURE["payload_len"]))
    image = build_image(payload, version=FIXTURE["version"], slot=FIXTURE["slot"], flags=FIXTURE["flags"])
    hdr = parse_image(image)
    digest = int.from_bytes(image[hdr["digestOffset"] : hdr["digestOffset"] + 4], "little")
    for label, got, want in (
        ("负载长度", hdr["imageSize"], FIXTURE["image_size"]),
        ("镜像总长", hdr["imageTotalSize"], FIXTURE["total_size"]),
        ("摘要偏移", hdr["digestOffset"], FIXTURE["digest_offset"]),
        ("摘要值", digest, FIXTURE["digest"]),
    ):
        if got != want:
            ok = False
            notes.append("FAIL 对拍基准 %s 不符：0x%X != 0x%X" % (label, got, want))
        else:
            notes.append("OK   对拍基准 %s = 0x%X" % (label, got))

    return ok, notes


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _fmt_flags(flags):
    names = [n for bit, n in FLAG_NAMES.items() if flags & bit]
    extra = flags & ~(F_VENDOR_BASE - 1)
    if extra:
        names.append("VENDOR(0x%X)" % extra)
    return "|".join(names) if names else "0"


def cmd_pack(args):
    payload = Path(args.input).read_bytes()
    flags = args.flags
    if flags is None:
        flags = F_SLOT_BOUND
    if args.no_slot_bound:
        flags &= ~F_SLOT_BOUND
    algo = {"crc32": DIGEST_CRC32_ISO_HDLC, "sha256": DIGEST_SHA256, "none": DIGEST_NONE}[args.algo]
    version = parse_version(args.version) if args.version else 0

    image = build_image(
        payload,
        version=version,
        slot=SLOT_ID[args.slot],
        algo=algo,
        flags=flags,
        payload_offset=args.payload_offset,
    )
    path = Path(args.output)
    path.write_bytes(image)

    h = parse_image(image)
    print("已写出 %s" % path)
    print("  负载偏移  0x%X（= app 链接偏移）" % h["payloadOffset"])
    print("  负载      %d 字节（补齐后 %d）" % (len(payload), h["imageSize"]))
    print("  镜像总长  %d 字节（含头/填充/摘要区）" % h["imageTotalSize"])
    print("  版本      0x%08X" % h["imageVersion"])
    print("  槽        %s（slotId=%d）" % (args.slot, h["slotId"]))
    print("  摘要      %s" % ALGO_NAMES[h["digestAlgo"]])

    ok, notes = verify_image(image)
    if not ok:
        print("内部错误：打包结果未通过自校验：%s" % notes, file=sys.stderr)
        return 1
    return 0


def cmd_inspect(args):
    data = Path(args.image).read_bytes()
    try:
        h = parse_image(data)
    except ImageError as exc:
        print("解析失败：%s" % exc, file=sys.stderr)
        return 1

    print("字段            值")
    print("magic           0x%08X%s" % (h["magic"], "" if h["magic"] == MAGIC else "   ← 非本框架镜像"))
    print("hdrVersion      %d" % h["hdrVersion"])
    print("hdrSize         %d" % h["hdrSize"])
    print("payloadOffset   0x%X" % h["payloadOffset"])
    print("imageSize       %d" % h["imageSize"])
    print("imageTotalSize  %d" % h["imageTotalSize"])
    print(
        "imageVersion    0x%08X  (%d.%d.%d)"
        % (
            h["imageVersion"],
            (h["imageVersion"] >> 24) & 0xFF,
            (h["imageVersion"] >> 16) & 0xFF,
            (h["imageVersion"] >> 8) & 0xFF,
        )
    )
    print("flags           0x%08X  (%s)" % (h["flags"], _fmt_flags(h["flags"])))
    print("digestAlgo      %d  (%s)" % (h["digestAlgo"], ALGO_NAMES.get(h["digestAlgo"], "未知")))
    print("digestLen       %d" % h["digestLen"])
    print("digestOffset    0x%X" % h["digestOffset"])
    print("digestRegionSize %d" % h["digestRegionSize"])
    print("slotId          %d" % h["slotId"])
    print("headerCrc32     0x%08X" % h["headerCrc32"])

    if h["digestLen"] and h["digestOffset"] + h["digestLen"] <= len(data):
        print("摘要值          %s" % data[h["digestOffset"] : h["digestOffset"] + h["digestLen"]].hex())
    return 0


def cmd_verify(args):
    data = Path(args.image).read_bytes()
    capacity = int(args.slot_capacity, 0) if args.slot_capacity else None
    ok, notes = verify_image(data, slot_capacity=capacity)
    for note in notes:
        print(("通过： " if ok else "拒绝： ") + note)
    return 0 if ok else 1


def cmd_selftest(_args):
    ok, notes = selftest()
    for note in notes:
        print(note)
    print("自检 %s" % ("通过" if ok else "失败"))
    return 0 if ok else 1


def main(argv=None):
    # Windows 控制台默认代码页不保证能编码中文，统一切到 UTF-8 并容错
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(encoding="utf-8", errors="replace")
            except (ValueError, OSError):
                pass

    loaded, message = load_contract()
    if not loaded:
        print("警告：%s" % message, file=sys.stderr)

    parser = argparse.ArgumentParser(description="镜像打包与检查（镜像格式契约的主机侧实现）")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("pack", help="由裸负载构造完整镜像")
    p.add_argument("--input", required=True, help="裸负载（.bin）")
    p.add_argument("--output", required=True, help="输出镜像路径")
    p.add_argument("--version", default="", help="版本号，如 1.2.3")
    p.add_argument("--slot", default="a", choices=sorted(SLOT_ID), help="槽标识")
    p.add_argument("--algo", default="crc32", choices=("crc32", "sha256", "none"), help="摘要算法")
    p.add_argument("--flags", type=lambda v: int(v, 0), default=None, help="flags 原始值（默认 SLOT_BOUND）")
    p.add_argument("--no-slot-bound", action="store_true", help="清除 SLOT_BOUND 位")
    p.add_argument(
        "--payload-offset",
        type=lambda v: int(v, 0),
        default=PAYLOAD_OFFSET,
        help="负载偏移，须 0x200 对齐且等于 app 链接偏移（默认 0x%X）" % PAYLOAD_OFFSET,
    )
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("inspect", help="打印头部字段")
    p.add_argument("image")
    p.set_defaults(func=cmd_inspect)

    p = sub.add_parser("verify", help="按契约判定顺序校验镜像")
    p.add_argument("image")
    p.add_argument("--slot-capacity", default=None, help="槽容量（字节，可用 0x 前缀）")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("selftest", help="算法自检向量 + 契约常量一致性 + 往返校验")
    p.set_defaults(func=cmd_selftest)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ImageError as exc:
        print("错误：%s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
