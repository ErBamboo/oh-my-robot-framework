#!/usr/bin/env python3
"""USB 继电器控制（CH340 串口，A0 帧协议）。

测试台上位机控制板的复位/上电：
- on/off：吸合/释放指定通道（吸合 = NO 与 COM 导通）
- pulse：吸合若干毫秒后释放；接 NRST-GND 时即一次复位
- cycle：释放并等待后再吸合；接电源串联时即一次断电重启
- query：查询通道状态（TC 系列返回 CHn:ON/OFF 文本）

帧格式：A0 <通道> <动作> <校验>，校验 = 前三字节之和的低 8 位。
通道 0x0F 表示全部；动作 00=关，01=开，02=查询。
TC 系列默认 115200，LCUS 系列默认 9600，用 --baud 指定。

用法：
    python tools/omrelay/relay.py ports
    python tools/omrelay/relay.py pulse --channel 1 --ms 150
    python tools/omrelay/relay.py cycle --channel 2 --off-ms 1000

串口未指定时自动选择唯一的 CH340（VID:PID 1A86:7523）。
"""

import argparse
import re
import sys
import time

CH340_VID = 0x1A86
CH340_PID = 0x7523
START = 0xA0
CHANNEL_ALL = 0x0F
ACTION_OFF = 0x00
ACTION_ON = 0x01
ACTION_QUERY = 0x02
VALID_ACTIONS = (ACTION_OFF, ACTION_ON, ACTION_QUERY)

STATUS_RE = re.compile(rb"CH(\d+):\s*(ON|OFF)")
STYLE_TC = "tc"
STYLE_LCUS = "lucs"
QUERY_STYLES = (STYLE_TC, STYLE_LCUS)


class RelayError(Exception):
    """串口不可用、参数非法或设备无响应。"""


def command_frame(channel, action):
    """构造 A0 协议帧；channel 为 1..8 或 0x0F。"""
    if channel != CHANNEL_ALL and not 1 <= channel <= 8:
        raise RelayError("通道必须是 1..8 或 0x0F（全部）")
    if action not in VALID_ACTIONS:
        raise RelayError("动作必须是 0x00/0x01/0x02")
    head = bytes((START, channel, action))
    return head + bytes((sum(head) & 0xFF,))


def on_frame(channel=1):
    return command_frame(channel, ACTION_ON)


def off_frame(channel=1):
    return command_frame(channel, ACTION_OFF)


def query_frame(channel=CHANNEL_ALL):
    return command_frame(channel, ACTION_QUERY)


def parse_status(data):
    """从 TC 系列应答中提取 {通道: 是否吸合}；无匹配时返回空字典。"""
    return {int(m.group(1)): m.group(2) == b"ON" for m in STATUS_RE.finditer(data)}


def default_query_style(baud):
    """按波特率选查询帧：LCUS 系列 9600 用 FF，TC 系列 115200 用 A0 帧。"""
    return STYLE_LCUS if baud <= 19200 else STYLE_TC


def list_ports():
    try:
        from serial.tools import list_ports as serial_list_ports
    except ImportError as exc:
        raise RelayError("需要 pyserial：pip install pyserial") from exc
    return list(serial_list_ports.comports())


def find_ch340_port():
    candidates = [p for p in list_ports() if p.vid == CH340_VID and p.pid == CH340_PID]
    if len(candidates) == 1:
        return candidates[0].device
    if not candidates:
        raise RelayError("未找到 CH340 串口，请用 --port 指定")
    names = "、".join(p.device for p in candidates)
    raise RelayError("检测到多个 CH340 串口（%s），请用 --port 指定" % names)


class Relay:
    """对已打开的串口对象发帧；串口对象需提供 write/flush/read/close。"""

    def __init__(self, port, wait=0.5):
        self._port = port
        self._wait = wait

    def send(self, frame):
        self._port.write(frame)
        if hasattr(self._port, "flush"):
            self._port.flush()

    def on(self, channel=1):
        self.send(on_frame(channel))

    def off(self, channel=1):
        self.send(off_frame(channel))

    def pulse(self, channel=1, ms=150):
        """吸合 ms 毫秒后释放；中途异常也保证释放。"""
        if ms < 0:
            raise RelayError("吸合时长不能为负")
        self.on(channel)
        try:
            time.sleep(ms / 1000.0)
        finally:
            self.off(channel)

    def cycle(self, channel=1, off_ms=1000, on_ms=300):
        """先释放等待，再吸合；结束保持吸合。"""
        if off_ms < 0 or on_ms < 0:
            raise RelayError("断电/上电时长不能为负")
        self.off(channel)
        time.sleep(off_ms / 1000.0)
        self.on(channel)
        time.sleep(on_ms / 1000.0)

    def query(self, channel=CHANNEL_ALL, wait=None, style=STYLE_TC):
        """查询状态并返回原始应答字节。

        TC 系列用 A0 帧；LCUS 系列用单字节 FF。两种帧不混发：LCUS 可能把
        FF 当作 A0 帧的校验字节，混发会导致查询无应答。查询不改变继电器状态。
        """
        wait = self._wait if wait is None else wait
        if wait < 0:
            raise RelayError("等待时长不能为负")
        if style not in QUERY_STYLES:
            raise RelayError("查询风格必须是 %s" % "/".join(QUERY_STYLES))
        if hasattr(self._port, "reset_input_buffer"):
            self._port.reset_input_buffer()
        self.send(b"\xFF" if style == STYLE_LCUS else query_frame(channel))
        data = bytearray()
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            chunk = self._port.read(64)
            if not chunk:
                break
            data.extend(chunk)
        return bytes(data)

    def close(self):
        if hasattr(self._port, "close"):
            self._port.close()


def open_relay(port, baud, timeout=0.5):
    try:
        import serial
    except ImportError as exc:
        raise RelayError("需要 pyserial：pip install pyserial") from exc
    try:
        handle = serial.Serial(
            port=port, baudrate=baud, timeout=timeout, write_timeout=1.0
        )
    except Exception as exc:  # pyserial 的异常类型随平台变化，统一收敛
        raise RelayError("无法打开串口 %s：%s" % (port, exc)) from exc
    # CH340 打开后立即发首帧偶尔被设备丢弃，留一个短建立时间
    time.sleep(0.05)
    return Relay(handle, wait=timeout)


def _print_ports():
    ports = list_ports()
    if not ports:
        print("未发现串口")
        return
    for port in ports:
        vid = "-" if port.vid is None else "%04X" % port.vid
        pid = "-" if port.pid is None else "%04X" % port.pid
        marker = " CH340" if port.vid == CH340_VID and port.pid == CH340_PID else ""
        print("%s  VID:PID=%s:%s%s  %s" % (port.device, vid, pid, marker, port.description))


def _print_query(data, baud, style):
    if not data:
        other = STYLE_TC if style == STYLE_LCUS else STYLE_LCUS
        print(
            "无应答（当前 %d/%s；可尝试 --style %s 或检查接线）"
            % (baud, style, other)
        )
        return
    print("原始: " + " ".join("%02X" % b for b in data))
    text = data.decode("ascii", errors="replace").strip()
    if text:
        print("文本: " + text.replace("\r", " ").replace("\n", " "))
    status = parse_status(data)
    if status:
        print("状态: " + " ".join(
            "CH%d=%s" % (ch, "ON" if on else "OFF") for ch, on in sorted(status.items())
        ))


def build_parser():
    parser = argparse.ArgumentParser(description="USB 继电器控制（CH340，A0 帧协议）")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("ports", help="列出串口并标记 CH340")

    def auto_int(text):
        return int(text, 0)

    def add_common(cmd):
        cmd.add_argument("--port", help="串口（如 COM9）；缺省自动选择唯一的 CH340")
        cmd.add_argument(
            "--baud", type=int, default=9600,
            help="波特率：LCUS 系列默认 9600，TC 系列用 115200",
        )
        cmd.add_argument(
            "--channel", type=auto_int, default=1,
            help="通道 1..8，0x0F 表示全部（默认 1）",
        )

    on = sub.add_parser("on", help="吸合通道")
    add_common(on)
    off = sub.add_parser("off", help="释放通道")
    add_common(off)
    pulse = sub.add_parser("pulse", help="吸合 ms 后释放（接 NRST 时即复位）")
    add_common(pulse)
    pulse.add_argument("--ms", type=int, default=150, help="吸合时长（默认 150 ms）")
    cycle = sub.add_parser("cycle", help="释放等待后吸合（接电源时即断电重启）")
    add_common(cycle)
    cycle.add_argument("--off-ms", type=int, default=1000, help="断电时长（默认 1000 ms）")
    cycle.add_argument("--on-ms", type=int, default=300, help="上电后保持（默认 300 ms）")
    query = sub.add_parser("query", help="查询通道状态")
    add_common(query)
    query.set_defaults(channel=CHANNEL_ALL)
    query.add_argument("--wait", type=float, default=0.5, help="等待应答秒数（默认 0.5）")
    query.add_argument(
        "--style", choices=("auto", STYLE_TC, STYLE_LCUS), default="auto",
        help="查询帧风格：auto 按波特率选择（9600→lucs，其余→tc）",
    )
    return parser


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(encoding="utf-8", errors="replace")
            except (ValueError, OSError):
                pass

    args = build_parser().parse_args(argv)
    try:
        if args.command == "ports":
            _print_ports()
            return 0
        port = args.port or find_ch340_port()
        relay = open_relay(port, args.baud)
        try:
            if args.command == "on":
                relay.on(args.channel)
                print("%s 通道 %d：吸合" % (port, args.channel))
            elif args.command == "off":
                relay.off(args.channel)
                print("%s 通道 %d：释放" % (port, args.channel))
            elif args.command == "pulse":
                relay.pulse(args.channel, args.ms)
                print("%s 通道 %d：吸合 %d ms 后释放" % (port, args.channel, args.ms))
            elif args.command == "cycle":
                relay.cycle(args.channel, args.off_ms, args.on_ms)
                print("%s 通道 %d：释放 %d ms 后吸合（保持）"
                      % (port, args.channel, args.off_ms))
            elif args.command == "query":
                style = args.style
                if style == "auto":
                    style = default_query_style(args.baud)
                data = relay.query(args.channel, args.wait, style)
                _print_query(data, args.baud, style)
        finally:
            relay.close()
        return 0
    except RelayError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
