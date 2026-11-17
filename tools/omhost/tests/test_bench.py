import json
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import bench


class FakeRelay:
    def __init__(self, replies):
        self.replies = list(replies)
        self.closed = False

    def query(self, channel, wait, style):
        return self.replies.pop(0) if self.replies else b""

    def close(self):
        self.closed = True


class StateTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="omhost-test-"))
        self.path = self.dir / "config.json"

    def test_defaults(self):
        state = bench.load_state(self.path)
        self.assertEqual(state["roles"], {"relay": "", "serial": "", "jlink": ""})
        self.assertEqual(state["settings"]["jlinkDevice"], "STM32F427IIHx")
        self.assertEqual(state["settings"]["relayBaud"], 9600)

    def test_save_and_reload(self):
        bench.save_state(
            self.path,
            {
                "roles": {"relay": " COM10 ", "serial": "COM9", "jlink": "63728769"},
                "settings": {"jlinkDevice": "STM32F427IIHx", "jlinkSpeed": 2000, "relayBaud": 9600},
            },
        )
        state = bench.load_state(self.path)
        self.assertEqual(state["roles"]["relay"], "COM10")
        self.assertEqual(state["roles"]["serial"], "COM9")
        self.assertEqual(state["settings"]["jlinkSpeed"], 2000)

    def test_invalid_state_is_rejected(self):
        with self.assertRaises(bench.BenchError):
            bench.save_state(self.path, {"roles": []})
        with self.assertRaises(bench.BenchError):
            bench.save_state(self.path, {"settings": {"jlinkDevice": "bad device"}})
        with self.assertRaises(bench.BenchError):
            bench.save_state(self.path, {"settings": {"relayBaud": 0}})


class JLinkListTest(unittest.TestCase):
    def test_parse_list_with_and_without_nickname(self):
        output = (
            "J-Link[0]: Connection: USB, Serial number: 63728769, "
            "ProductName: J-Link CE, Nickname: J-Link V9.7 Plus\n"
            "J-Link[1]: Connection: USB, Serial number: 12345678, "
            "ProductName: J-Link OB\n"
        )
        probes = bench.parse_jlink_list(output)
        self.assertEqual(len(probes), 2)
        self.assertEqual(probes[0]["serial"], "63728769")
        self.assertEqual(probes[0]["nickname"], "J-Link V9.7 Plus")
        self.assertEqual(probes[1]["serial"], "12345678")
        self.assertEqual(probes[1]["nickname"], "")


class ResourceTest(unittest.TestCase):
    def test_present_marks_assigned_resources(self):
        path = Path(tempfile.mkdtemp(prefix="omhost-test-")) / "config.json"
        bench.save_state(
            path,
            {
                "roles": {"relay": "COM10", "serial": "COM9", "jlink": "63728769"},
                "settings": bench.DEFAULT_SETTINGS,
            },
        )
        serial = [
            {"device": "COM9", "vid": None, "pid": None, "description": "", "manufacturer": "", "isCh340": False},
            {"device": "COM10", "vid": 0x1A86, "pid": 0x7523, "description": "", "manufacturer": "", "isCh340": True},
        ]
        jlink = [{"index": 0, "serial": "63728769", "product": "J-Link CE", "nickname": ""}]
        with mock.patch.object(bench, "probe_serial_ports", return_value=(serial, "")), \
             mock.patch.object(bench, "probe_jlink_probes", return_value=(jlink, "")):
            payload = bench.resources_payload(path)
        self.assertTrue(payload["present"]["relay"]["present"])
        self.assertTrue(payload["present"]["serial"]["present"])
        self.assertTrue(payload["present"]["jlink"]["present"])

    def test_probe_relay_parses_spaced_status(self):
        fake = FakeRelay([b"CH1: OFF\r\nCH2: ON\r\nCH3: OFF\r\nCH4: OFF\r\n"])
        with mock.patch.object(bench.relay, "open_relay", return_value=fake):
            payload = bench.probe_relay("COM10", 9600)
        self.assertEqual(
            payload["status"], {"1": False, "2": True, "3": False, "4": False}
        )
        self.assertTrue(fake.closed)

    def test_probe_relay_retries_once(self):
        first = FakeRelay([b""])
        second = FakeRelay([b"CH1: ON\r\n"])
        with mock.patch.object(bench.relay, "open_relay", side_effect=[first, second]), \
             mock.patch.object(bench.time, "sleep"):
            payload = bench.probe_relay("COM10", 9600)
        self.assertEqual(payload["status"], {"1": True})
        self.assertTrue(first.closed)
        self.assertTrue(second.closed)

    def test_probe_relay_no_response_is_rejected(self):
        fakes = [FakeRelay([b""]), FakeRelay([b""])]
        with mock.patch.object(bench.relay, "open_relay", side_effect=fakes), \
             mock.patch.object(bench.time, "sleep"):
            with self.assertRaises(bench.BenchError):
                bench.probe_relay("COM10", 9600)


class JLinkResetTest(unittest.TestCase):
    def test_script_and_arguments(self):
        captured = {}

        def fake_run(cmd, capture_output, text, timeout):
            script_path = Path(cmd[cmd.index("-CommandFile") + 1])
            captured["cmd"] = cmd
            captured["script"] = script_path.read_text(encoding="utf-8")
            return SimpleNamespace(returncode=0, stdout="Script processing completed.", stderr="")

        with mock.patch.object(bench.subprocess, "run", side_effect=fake_run):
            output = bench.jlink_reset("JLink.exe", "63728769", run=False)
        self.assertIn("Script processing completed.", output)
        self.assertIn("-SelectEmuBySN", captured["cmd"])
        self.assertIn("63728769", captured["cmd"])
        lines = captured["script"].splitlines()
        self.assertIn("device STM32F427IIHx", lines)
        self.assertIn("si SWD", lines)
        self.assertIn("connect", lines)
        self.assertIn("r", lines)
        self.assertNotIn("g", lines)

    def test_run_true_adds_go(self):
        captured = {}

        def fake_run(cmd, capture_output, text, timeout):
            script_path = Path(cmd[cmd.index("-CommandFile") + 1])
            captured["script"] = script_path.read_text(encoding="utf-8")
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        with mock.patch.object(bench.subprocess, "run", side_effect=fake_run):
            bench.jlink_reset("JLink.exe", "63728769", run=True)
        self.assertIn("g", captured["script"].splitlines())

    def test_invalid_serial_is_rejected(self):
        with self.assertRaises(bench.BenchError):
            bench.jlink_reset("JLink.exe", "not-a-serial")


class TestCaseLoaderTest(unittest.TestCase):
    def _write(self, directory, data):
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / (data["id"] + ".json")
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def test_load_and_roles(self):
        directory = Path(tempfile.mkdtemp(prefix="omhost-tests-"))
        self._write(
            directory,
            {
                "schema": 1,
                "id": "relay-case",
                "name": "relay",
                "steps": [{"type": "relay_pulse", "channel": 1, "ms": 10}],
            },
        )
        self._write(
            directory,
            {
                "schema": 1,
                "id": "blocked-case",
                "name": "blocked",
                "blocked": "OMR-49",
                "steps": [{"type": "serial_download", "slot": "a"}],
            },
        )
        cases = bench.load_tests([directory])
        self.assertEqual([case["id"] for case in cases], ["blocked-case", "relay-case"])
        self.assertEqual(cases[1]["roles"], ["relay"])
        self.assertEqual(cases[0]["roles"], ["serial"])

    def test_duplicate_id_is_rejected(self):
        first = Path(tempfile.mkdtemp(prefix="omhost-tests-"))
        second = Path(tempfile.mkdtemp(prefix="omhost-tests-"))
        data = {"schema": 1, "id": "dup", "name": "dup", "steps": [{"type": "delay", "ms": 1}]}
        self._write(first, data)
        self._write(second, data)
        with self.assertRaises(bench.BenchError):
            bench.load_tests([first, second])

    def test_unknown_step_is_rejected(self):
        directory = Path(tempfile.mkdtemp(prefix="omhost-tests-"))
        self._write(
            directory,
            {"schema": 1, "id": "bad", "name": "bad", "steps": [{"type": "shell"}]},
        )
        with self.assertRaises(bench.BenchError):
            bench.load_tests([directory])


class BuiltinCasesTest(unittest.TestCase):
    def test_builtin_cases_load(self):
        cases = bench.load_tests([bench.HERE / "testcases"])
        ids = {case["id"] for case in cases}
        self.assertIn("host-layout-image-a", ids)
        self.assertIn("bench-relay-pulse", ids)
        self.assertIn("bench-jlink-reset", ids)
        self.assertIn("ota-download-a", ids)
        blocked = {case["id"]: case.get("blocked", "") for case in cases}
        self.assertEqual(blocked["ota-download-a"], "OMR-49")
        self.assertEqual(blocked["ota-resume-b"], "OMR-49")


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="omhost-run-"))
        self.state = self.dir / "config.json"
        self.tests = self.dir / "tests"
        self.tests.mkdir()
        self.layout = bench._repo_root() / "samples/boot/layout/rm-a-board/om_bootcfg.json"
        self.assertTrue(self.layout.is_file())

    def _case(self, data):
        path = self.tests / (data["id"] + ".json")
        path.write_text(json.dumps(data), encoding="utf-8")

    def _wait(self, manager, run_id):
        for _ in range(100):
            time.sleep(0.05)
            run = manager.get_run(run_id)
            if run["status"] != "running":
                return run
        self.fail("run did not finish")

    def test_host_only_case_passes(self):
        self._case(
            {
                "schema": 1,
                "id": "host",
                "name": "host",
                "steps": [
                    {"type": "layout_validate", "layout": self.layout.as_posix()},
                    {
                        "type": "image_pack",
                        "layout": self.layout.as_posix(),
                        "slot": "a",
                        "payloadText": "sample",
                        "output": "sample.img.bin",
                    },
                    {
                        "type": "image_inspect",
                        "layout": self.layout.as_posix(),
                        "image": "sample.img.bin",
                    },
                ],
            }
        )
        manager = bench.RunManager(self.state, [self.tests])
        run = manager.start("host")
        finished = self._wait(manager, run["id"])
        self.assertEqual(finished["status"], "passed", finished["error"])
        self.assertEqual(len(finished["artifacts"]), 1)
        self.assertTrue(Path(finished["artifacts"][0]).is_file())

    def test_missing_roles_are_rejected(self):
        self._case(
            {
                "schema": 1,
                "id": "relay",
                "name": "relay",
                "steps": [{"type": "relay_pulse", "channel": 1, "ms": 10}],
            }
        )
        manager = bench.RunManager(self.state, [self.tests])
        with self.assertRaises(bench.BenchError):
            manager.start("relay")

    def test_blocked_case_is_rejected(self):
        self._case(
            {
                "schema": 1,
                "id": "blocked",
                "name": "blocked",
                "blocked": "OMR-49",
                "steps": [{"type": "serial_download", "slot": "a"}],
            }
        )
        bench.save_state(
            self.state,
            {"roles": {"relay": "", "serial": "COM9", "jlink": ""}},
        )
        manager = bench.RunManager(self.state, [self.tests])
        with self.assertRaises(bench.BenchError):
            manager.start("blocked")

    def test_artifact_path_rejects_escape(self):
        with self.assertRaises(bench.BenchError):
            bench._artifact_path(self.dir, "../evil.bin")
        with self.assertRaises(bench.BenchError):
            bench._artifact_path(self.dir, str(self.dir / "abs.bin"))


if __name__ == "__main__":
    unittest.main()
