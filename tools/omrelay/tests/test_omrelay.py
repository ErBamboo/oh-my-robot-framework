import unittest
from unittest import mock

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import relay


class FakePort:
    def __init__(self, replies=()):
        self.writes = []
        self._replies = list(replies)
        self.flushed = 0
        self.reset_count = 0
        self.closed = False

    def write(self, data):
        self.writes.append(bytes(data))
        return len(data)

    def flush(self):
        self.flushed += 1

    def read(self, size):
        if self._replies:
            return self._replies.pop(0)
        return b""

    def reset_input_buffer(self):
        self.reset_count += 1

    def close(self):
        self.closed = True


class FrameTest(unittest.TestCase):
    def test_on_off_frames_match_protocol(self):
        self.assertEqual(relay.on_frame(1), bytes.fromhex("A0 01 01 A2"))
        self.assertEqual(relay.off_frame(1), bytes.fromhex("A0 01 00 A1"))
        self.assertEqual(relay.on_frame(2), bytes.fromhex("A0 02 01 A3"))
        self.assertEqual(relay.off_frame(4), bytes.fromhex("A0 04 00 A4"))
        self.assertEqual(relay.on_frame(relay.CHANNEL_ALL), bytes.fromhex("A0 0F 01 B0"))
        self.assertEqual(relay.off_frame(relay.CHANNEL_ALL), bytes.fromhex("A0 0F 00 AF"))

    def test_query_frames_match_protocol(self):
        self.assertEqual(relay.query_frame(1), bytes.fromhex("A0 01 02 A3"))
        self.assertEqual(relay.query_frame(4), bytes.fromhex("A0 04 02 A6"))
        self.assertEqual(
            relay.query_frame(relay.CHANNEL_ALL), bytes.fromhex("A0 0F 02 B1")
        )

    def test_invalid_channel_is_rejected(self):
        for channel in (0, 9, 0x10):
            with self.assertRaises(relay.RelayError):
                relay.on_frame(channel)

    def test_invalid_action_is_rejected(self):
        with self.assertRaises(relay.RelayError):
            relay.command_frame(1, 0x03)


class RelayTest(unittest.TestCase):
    def test_on_off_write_expected_frames(self):
        port = FakePort()
        dev = relay.Relay(port)
        dev.on(3)
        dev.off(3)
        self.assertEqual(
            port.writes, [bytes.fromhex("A0 03 01 A4"), bytes.fromhex("A0 03 00 A3")]
        )
        self.assertEqual(port.flushed, 2)

    def test_pulse_is_on_then_off(self):
        port = FakePort()
        dev = relay.Relay(port)
        with mock.patch.object(relay.time, "sleep") as sleep:
            dev.pulse(1, 120)
        sleep.assert_called_once_with(0.12)
        self.assertEqual(
            port.writes, [bytes.fromhex("A0 01 01 A2"), bytes.fromhex("A0 01 00 A1")]
        )

    def test_pulse_releases_on_interrupt(self):
        port = FakePort()
        dev = relay.Relay(port)
        with mock.patch.object(relay.time, "sleep", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                dev.pulse(1, 100)
        self.assertEqual(
            port.writes, [bytes.fromhex("A0 01 01 A2"), bytes.fromhex("A0 01 00 A1")]
        )

    def test_cycle_is_off_wait_on_wait(self):
        port = FakePort()
        dev = relay.Relay(port)
        with mock.patch.object(relay.time, "sleep") as sleep:
            dev.cycle(2, 800, 200)
        self.assertEqual(
            port.writes, [bytes.fromhex("A0 02 00 A2"), bytes.fromhex("A0 02 01 A3")]
        )
        self.assertEqual([c.args[0] for c in sleep.call_args_list], [0.8, 0.2])

    def test_query_sends_tc_frame(self):
        port = FakePort(replies=[b"CH1:ON\r\nCH2:OFF\r\n"])
        dev = relay.Relay(port, wait=0.1)
        data = dev.query(style=relay.STYLE_TC)
        self.assertEqual(port.writes, [bytes.fromhex("A0 0F 02 B1")])
        self.assertEqual(data, b"CH1:ON\r\nCH2:OFF\r\n")
        self.assertEqual(port.reset_count, 1)

    def test_query_sends_lcus_frame(self):
        port = FakePort(replies=[b"CH1:ON\r\nCH2:OFF\r\n"])
        dev = relay.Relay(port, wait=0.1)
        data = dev.query(style=relay.STYLE_LCUS)
        self.assertEqual(port.writes, [b"\xFF"])
        self.assertEqual(data, b"CH1:ON\r\nCH2:OFF\r\n")

    def test_query_style_follows_baud(self):
        self.assertEqual(relay.default_query_style(9600), relay.STYLE_LCUS)
        self.assertEqual(relay.default_query_style(115200), relay.STYLE_TC)

    def test_negative_durations_and_bad_style_are_rejected(self):
        dev = relay.Relay(FakePort())
        with self.assertRaises(relay.RelayError):
            dev.pulse(1, -1)
        with self.assertRaises(relay.RelayError):
            dev.cycle(1, -1, 100)
        with self.assertRaises(relay.RelayError):
            dev.query(wait=-1)
        with self.assertRaises(relay.RelayError):
            dev.query(style="unknown")


class StatusTest(unittest.TestCase):
    def test_parse_status(self):
        data = b"CH1:ON\r\nCH2:OFF\r\nCH3:OFF\r\n"
        self.assertEqual(
            relay.parse_status(data), {1: True, 2: False, 3: False}
        )

    def test_parse_status_with_spaces(self):
        data = b"CH1: OFF\r\nCH2: ON\r\nCH3: OFF\r\nCH4: OFF\r\n"
        self.assertEqual(
            relay.parse_status(data),
            {1: False, 2: True, 3: False, 4: False},
        )

    def test_parse_status_without_text(self):
        self.assertEqual(relay.parse_status(bytes.fromhex("01 01 00 00")), {})


class PortLookupTest(unittest.TestCase):
    class FakeInfo:
        def __init__(self, device, vid, pid):
            self.device = device
            self.vid = vid
            self.pid = pid
            self.description = device

    def test_single_ch340_is_selected(self):
        infos = [
            self.FakeInfo("COM1", None, None),
            self.FakeInfo("COM9", relay.CH340_VID, relay.CH340_PID),
        ]
        with mock.patch.object(relay, "list_ports", return_value=infos):
            self.assertEqual(relay.find_ch340_port(), "COM9")

    def test_no_ch340_is_rejected(self):
        with mock.patch.object(relay, "list_ports", return_value=[]):
            with self.assertRaises(relay.RelayError):
                relay.find_ch340_port()

    def test_multiple_ch340_is_rejected(self):
        infos = [
            self.FakeInfo("COM8", relay.CH340_VID, relay.CH340_PID),
            self.FakeInfo("COM9", relay.CH340_VID, relay.CH340_PID),
        ]
        with mock.patch.object(relay, "list_ports", return_value=infos):
            with self.assertRaises(relay.RelayError):
                relay.find_ch340_port()


if __name__ == "__main__":
    unittest.main()
