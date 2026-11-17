from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(TOOL_DIR))

import omlayout  # noqa: E402


class LayoutToolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.spec_path = REPO_ROOT / "samples" / "boot" / "layout" / "rm-a-board" / "om_bootcfg.json"
        with cls.spec_path.open("rb") as stream:
            cls.raw = json.load(stream)
        cls.layout = omlayout.load_layout(cls.spec_path)

    def assert_layout_error(self, raw: dict, needle: str) -> None:
        with self.assertRaises(omlayout.LayoutError) as caught:
            omlayout.parse_layout(raw, "<test>")
        self.assertIn(needle, str(caught.exception))

    def test_schema_file_is_valid_json(self) -> None:
        schema_path = TOOL_DIR / "om_bootcfg.schema.json"
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")
        self.assertEqual(schema["properties"]["schema"]["const"], 1)
        self.assertEqual(schema["properties"]["metaCopies"]["const"], 2)

    def test_sample_is_valid(self) -> None:
        layout = self.layout
        self.assertEqual(layout.board, "rm-a-board")
        self.assertEqual(layout.flash_base, 0x08000000)
        self.assertEqual(layout.flash_size, 0x00200000)
        self.assertEqual(layout.payload_offset, 0x200)
        self.assertEqual(layout.meta_copies, 2)
        self.assertEqual(len(layout.partitions), 5)
        self.assertEqual(len(layout.reserved), 1)
        self.assertEqual(layout.fingerprint, int(layout.layout_sha256[:4], 16))
        slots = {p.slot: p for p in layout.partitions if p.role == omlayout.ROLE_APP}
        self.assertEqual(set(slots), {"a", "b"})
        self.assertEqual(slots["a"].link_base, 0x08010200)
        self.assertEqual(slots["a"].vtor_offset, 0x10200)
        self.assertEqual(slots["b"].link_base, 0x08110200)
        self.assertEqual(slots["b"].vtor_offset, 0x110200)
        self.assertEqual(slots["a"].size, slots["b"].size)

    def test_generate_outputs_and_determinism(self) -> None:
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            outputs_a = omlayout.write_outputs(self.layout, Path(first))
            outputs_b = omlayout.write_outputs(self.layout, Path(second))
            self.assertEqual(
                [path.name for path in outputs_a],
                [
                    "om_bootcfg.h",
                    "boot_partition_data.c",
                    "boot_partition_data.h",
                    "om_bootcfg.generated.json",
                ],
            )
            for path_a, path_b in zip(outputs_a, outputs_b):
                self.assertEqual(path_a.read_bytes(), path_b.read_bytes())

            header = (Path(first) / "om_bootcfg.h").read_text(encoding="utf-8")
            self.assertIn("#define OM_BOOT_PARTITIONS", header)
            self.assertIn("#define OM_IMAGE_PAYLOAD_OFFSET 0x00000200u", header)
            self.assertIn("#define OM_BOOT_META_COPIES 2u", header)
            self.assertIn("#define OM_BOOT_SLOT_A_LINK_BASE 0x08010200u", header)
            self.assertIn("#define OM_BOOT_SLOT_B_VTOR_OFFSET 0x00110200u", header)

            manifest = json.loads(
                (Path(first) / "om_bootcfg.generated.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["board"], "rm-a-board")
            self.assertEqual(manifest["slots"]["a"]["partition"], "app_a")
            self.assertEqual(manifest["slots"]["a"]["size"], 0xF0000)
            self.assertEqual(manifest["meta"]["eraseUnitCount"], 2)
            self.assertEqual(manifest["fingerprint"], self.layout.fingerprint)

    def test_generate_linker_scripts(self) -> None:
        template_root = REPO_ROOT / "platform" / "bsp" / "boards" / "rm-a-board" / "linker" / "templates"
        with tempfile.TemporaryDirectory() as output:
            out_dir = Path(output)
            outputs = omlayout.write_outputs(self.layout, out_dir, template_root)
            names = {path.relative_to(out_dir).as_posix() for path in outputs}
            self.assertIn("linker/gcc/bootloader.ld", names)
            self.assertIn("linker/gcc/app_a.ld", names)
            self.assertIn("linker/gcc/app_b.ld", names)
            self.assertIn("linker/arm/bootloader.sct", names)
            self.assertIn("linker/arm/app_a.sct", names)
            self.assertIn("linker/arm/app_b.sct", names)

            gcc_app_a = (out_dir / "linker" / "gcc" / "app_a.ld").read_text(encoding="utf-8")
            gcc_app_b = (out_dir / "linker" / "gcc" / "app_b.ld").read_text(encoding="utf-8")
            arm_boot = (out_dir / "linker" / "arm" / "bootloader.sct").read_text(encoding="utf-8")
            self.assertIn("ORIGIN = 0x08010200,   LENGTH = 0x000EFE00", gcc_app_a)
            self.assertIn("ORIGIN = 0x08110200,   LENGTH = 0x000EFE00", gcc_app_b)
            self.assertIn("LR_IROM1 0x08000000 0x00010000", arm_boot)

            messages = omlayout.check_linkers(self.layout, out_dir / "linker")
            self.assertEqual(len(messages), 6)

    def test_contract_payload_offset_matches_layout(self) -> None:
        contract = REPO_ROOT / "lib" / "boot" / "include" / "boot" / "image.h"
        message = omlayout.check_contract(self.layout, contract)
        self.assertIn("payload offset", message)

    def test_overlap_is_rejected(self) -> None:
        raw = copy.deepcopy(self.raw)
        raw["reserved"][0]["offset"] = 0x108000
        self.assert_layout_error(raw, "overlaps")

    def test_unaligned_partition_is_rejected(self) -> None:
        raw = copy.deepcopy(self.raw)
        raw["partitions"][0]["offset"] = 0x100
        self.assert_layout_error(raw, "aligned")

    def test_meta_capacity_is_rejected(self) -> None:
        raw = copy.deepcopy(self.raw)
        raw["partitions"][2]["size"] = 0x4000
        self.assert_layout_error(raw, "erase units")

    def test_meta_copies_must_be_two(self) -> None:
        raw = copy.deepcopy(self.raw)
        raw["metaCopies"] = 1
        self.assert_layout_error(raw, "metaCopies")

    def test_app_slot_sizes_must_match(self) -> None:
        raw = copy.deepcopy(self.raw)
        raw["partitions"][4]["size"] = 0xD0000
        self.assert_layout_error(raw, "same size")

    def test_duplicate_partition_name_is_rejected(self) -> None:
        raw = copy.deepcopy(self.raw)
        raw["partitions"][1]["name"] = raw["partitions"][0]["name"]
        self.assert_layout_error(raw, "duplicate")

    def test_payload_offset_is_aligned(self) -> None:
        raw = copy.deepcopy(self.raw)
        raw["payloadOffset"] = 0x100
        self.assert_layout_error(raw, "0x200")


if __name__ == "__main__":
    unittest.main()
