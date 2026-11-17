#!/usr/bin/env python3
"""omhost 测试台：资源探测、角色分配与测试矩阵执行。

资源由后端探测，角色由用户分配并保存到用户目录（默认 ~/.omhost/config.json）：
- relay：USB 继电器串口（tools/omrelay）
- serial：被测板串口（串口下载/日志，协议落地后使用）
- jlink：J-Link 序列号（软复位/调试）

测试矩阵是 tools/omhost/testcases/*.json 的数据文件；每个文件一个用例，
步骤按顺序执行，日志与产物保存在 ~/.omhost/runs/<run-id>/。
"""

import copy
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "omimg"))
sys.path.insert(0, str(HERE.parent / "omrelay"))

import omimg  # noqa: E402  （路径注入须先于导入）
import omlayout  # noqa: E402
import relay  # noqa: E402

ROLE_RELAY = "relay"
ROLE_SERIAL = "serial"
ROLE_JLINK = "jlink"
ROLE_LABELS = {
    ROLE_RELAY: "继电器",
    ROLE_SERIAL: "被测板串口",
    ROLE_JLINK: "J-Link",
}
DEFAULT_SETTINGS = {
    "jlinkDevice": "STM32F427IIHx",
    "jlinkSpeed": 4000,
    "relayBaud": 9600,
}
STEP_ROLES = {
    "relay_pulse": {ROLE_RELAY},
    "relay_cycle": {ROLE_RELAY},
    "relay_set": {ROLE_RELAY},
    "relay_query": {ROLE_RELAY},
    "jlink_reset": {ROLE_JLINK},
    "serial_download": {ROLE_SERIAL},
}
STEP_TYPES = {
    "delay",
    "relay_pulse",
    "relay_cycle",
    "relay_set",
    "relay_query",
    "jlink_reset",
    "layout_validate",
    "image_pack",
    "image_inspect",
    "serial_download",
}
ALGO_MAP = {
    "crc32": omimg.DIGEST_CRC32_ISO_HDLC,
    "sha256": omimg.DIGEST_SHA256,
    "none": omimg.DIGEST_NONE,
}
JLINK_LINE_RE = re.compile(
    r"J-Link\[(\d+)\]:.*Serial number:\s*(\d+).*ProductName:\s*([^,]+)"
    r"(?:,\s*Nickname:\s*(.*))?\s*$"
)


class BenchError(Exception):
    """资源不可用、配置非法或测试用例不合法。"""


class StepBlocked(BenchError):
    """步骤依赖尚未实现的能力。"""


def default_state_path():
    return Path.home() / ".omhost" / "config.json"


def default_test_dirs():
    dirs = [HERE / "testcases"]
    project = Path.cwd() / "cfg" / "omhost" / "testcases"
    if project not in dirs:
        dirs.append(project)
    return dirs


def _repo_root():
    return HERE.parents[1]


def default_state():
    return {
        "schema": 1,
        "roles": {role: "" for role in ROLE_LABELS},
        "settings": dict(DEFAULT_SETTINGS),
    }


def load_state(path):
    path = Path(path)
    if not path.is_file():
        return default_state()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise BenchError("无法读取资源分配 %s：%s" % (path, exc)) from exc
    except json.JSONDecodeError as exc:
        raise BenchError("资源分配 JSON 无效 %s：%s" % (path, exc)) from exc
    return _sanitize_state(data)


def _sanitize_state(data):
    if not isinstance(data, dict):
        raise BenchError("资源分配顶层必须是对象")
    state = default_state()
    roles = data.get("roles", {})
    if not isinstance(roles, dict):
        raise BenchError("roles 必须是对象")
    for role in ROLE_LABELS:
        value = roles.get(role, "")
        if not isinstance(value, str):
            raise BenchError("角色 %s 必须是字符串" % role)
        state["roles"][role] = value.strip()

    settings = data.get("settings", {})
    if not isinstance(settings, dict):
        raise BenchError("settings 必须是对象")
    device = settings.get("jlinkDevice", DEFAULT_SETTINGS["jlinkDevice"])
    if not isinstance(device, str) or not re.match(r"^[A-Za-z0-9_-]+$", device.strip()):
        raise BenchError("jlinkDevice 只允许字母、数字、下划线和连字符")
    state["settings"]["jlinkDevice"] = device.strip()
    for key in ("jlinkSpeed", "relayBaud"):
        value = settings.get(key, DEFAULT_SETTINGS[key])
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise BenchError("%s 必须是正整数" % key)
        state["settings"][key] = value
    return state


def save_state(path, data):
    state = _sanitize_state(data)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    text = json.dumps(state, indent=2, ensure_ascii=False) + "\n"
    try:
        temp.write_text(text, encoding="utf-8", newline="\n")
        os.replace(temp, path)
    except OSError as exc:
        raise BenchError("写入资源分配失败 %s：%s" % (path, exc)) from exc
    return state


# ---------------------------------------------------------------- 资源探测


def probe_serial_ports():
    try:
        from serial.tools import list_ports as serial_list_ports
    except ImportError:
        return [], "未安装 pyserial：pip install pyserial"
    ports = []
    for port in sorted(serial_list_ports.comports(), key=lambda item: item.device):
        ports.append(
            {
                "device": port.device,
                "vid": port.vid,
                "pid": port.pid,
                "description": port.description or "",
                "manufacturer": port.manufacturer or "",
                "isCh340": port.vid == relay.CH340_VID and port.pid == relay.CH340_PID,
            }
        )
    return ports, ""


def find_jlink_exe():
    override = os.environ.get("JLINK_EXE")
    if override and Path(override).is_file():
        return override
    for name in ("JLink.exe", "JLinkExe", "JLink"):
        found = shutil.which(name)
        if found:
            return found
    patterns = []
    for env_name in ("ProgramFiles", "ProgramFiles(x86)"):
        base = os.environ.get(env_name)
        if base:
            patterns.append(os.path.join(base, "SEGGER", "JLink*", "JLink.exe"))
    for pattern in patterns:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[-1]
    raise BenchError("未找到 JLink.exe；请加入 PATH 或设置 JLINK_EXE")


def parse_jlink_list(output):
    probes = []
    for line in output.splitlines():
        match = JLINK_LINE_RE.search(line.strip())
        if not match:
            continue
        probes.append(
            {
                "index": int(match.group(1)),
                "serial": match.group(2),
                "product": match.group(3).strip(),
                "nickname": (match.group(4) or "").strip(),
            }
        )
    return probes


def probe_jlink_probes():
    try:
        exe = find_jlink_exe()
    except BenchError as exc:
        return [], str(exc)
    script = "ShowEmuList\nexit\n"
    fd, path = tempfile.mkstemp(suffix=".jlink", text=True)
    try:
        os.write(fd, script.encode("utf-8"))
        os.close(fd)
        proc = subprocess.run(
            [exe, "-NoGui", "1", "-ExitOnError", "1", "-CommandFile", path],
            capture_output=True,
            text=True,
            timeout=20,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return [], "J-Link 探测失败：%s" % exc
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    output = (proc.stdout or "") + (proc.stderr or "")
    probes = parse_jlink_list(output)
    if proc.returncode != 0 and not probes:
        return [], "J-Link 探测失败：%s" % _tail(output)
    return probes, ""


def resources_payload(state_path):
    state = load_state(state_path)
    serial_ports, serial_error = probe_serial_ports()
    jlink_probes, jlink_error = probe_jlink_probes()
    errors = [message for message in (serial_error, jlink_error) if message]

    present = {}
    for role, value in state["roles"].items():
        if not value:
            continue
        if role == ROLE_JLINK:
            found = any(probe["serial"] == value for probe in jlink_probes)
        else:
            found = any(port["device"] == value for port in serial_ports)
        present[role] = {"value": value, "present": found}
    return {
        "ok": True,
        "serial": serial_ports,
        "jlink": jlink_probes,
        "roles": state["roles"],
        "settings": state["settings"],
        "present": present,
        "errors": errors,
    }


def probe_relay(port, baud):
    if not port:
        raise BenchError("未分配继电器串口")
    data = b""
    for attempt in range(2):
        try:
            dev = relay.open_relay(port, baud)
        except relay.RelayError as exc:
            raise BenchError(str(exc)) from exc
        try:
            data = dev.query(
                relay.CHANNEL_ALL, 0.8, relay.default_query_style(baud)
            )
        finally:
            dev.close()
        status = relay.parse_status(data)
        if status:
            return {
                "ok": True,
                "port": port,
                "baud": baud,
                "raw": data.hex(),
                "status": {
                    str(channel): bool(on) for channel, on in sorted(status.items())
                },
            }
        if attempt == 0:
            time.sleep(0.15)  # CH340 关闭后立即重开时偶发丢首帧
    raise BenchError("继电器无应答（检查端口、波特率与型号）")


# ---------------------------------------------------------------- J-Link


def jlink_reset(exe, serial, device="STM32F427IIHx", speed=4000, run=True):
    if not re.match(r"^\d+$", serial or ""):
        raise BenchError("J-Link 序列号必须是数字")
    if not re.match(r"^[A-Za-z0-9_-]+$", device or ""):
        raise BenchError("J-Link 器件名只允许字母、数字、下划线和连字符")
    if isinstance(speed, bool) or not isinstance(speed, int) or speed <= 0:
        raise BenchError("J-Link 速度必须是正整数 kHz")
    script = (
        "device %s\n"
        "si SWD\n"
        "speed %d\n"
        "connect\n"
        "r\n"
        "%s"
        "exit\n" % (device, speed, "g\n" if run else "")
    )
    fd, path = tempfile.mkstemp(suffix=".jlink", text=True)
    try:
        os.write(fd, script.encode("utf-8"))
        os.close(fd)
        proc = subprocess.run(
            [
                exe,
                "-NoGui",
                "1",
                "-ExitOnError",
                "1",
                "-SelectEmuBySN",
                serial,
                "-CommandFile",
                path,
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise BenchError("J-Link 复位失败：%s" % exc) from exc
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        raise BenchError("J-Link 复位失败：%s" % _tail(output))
    return output


def _tail(text, lines=8):
    rows = [row for row in (text or "").splitlines() if row.strip()]
    return "\n".join(rows[-lines:])


# ---------------------------------------------------------------- 测试用例


def required_roles(case):
    roles = set()
    for step in case["steps"]:
        roles.update(STEP_ROLES.get(step["type"], set()))
    return roles


def _load_layout_file(path):
    try:
        return omlayout.load_layout(path)
    except omlayout.LayoutError as exc:
        raise BenchError(str(exc)) from exc


def _resolve_input(text, case_dir):
    path = Path(text)
    if path.is_absolute():
        candidates = [path]
    else:
        candidates = [case_dir / path, _repo_root() / path, Path.cwd() / path]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise BenchError("找不到文件：%s" % text)


def _artifact_path(run_dir, text):
    path = Path(text)
    if path.is_absolute() or ".." in path.parts:
        raise BenchError("产物路径必须是运行目录内的相对路径：%s" % text)
    out = run_dir / path
    out.parent.mkdir(parents=True, exist_ok=True)
    return out


def _validate_case(data, path):
    if not isinstance(data, dict):
        raise BenchError("%s：顶层必须是对象" % path)
    if data.get("schema") != 1:
        raise BenchError("%s：不支持的 schema" % path)
    test_id = data.get("id")
    if not isinstance(test_id, str) or not re.match(r"^[A-Za-z0-9_.-]+$", test_id):
        raise BenchError("%s：id 非法" % path)
    if not isinstance(data.get("name"), str) or not data["name"].strip():
        raise BenchError("%s：name 不能为空" % path)
    if "blocked" in data and (
        not isinstance(data["blocked"], str) or not data["blocked"].strip()
    ):
        raise BenchError("%s：blocked 必须是非空字符串" % path)
    steps = data.get("steps")
    if not isinstance(steps, list) or not steps:
        raise BenchError("%s：steps 必须是非空数组" % path)
    for index, step in enumerate(steps):
        if not isinstance(step, dict) or step.get("type") not in STEP_TYPES:
            raise BenchError("%s：steps[%d] 类型未知" % (path, index))
        if "channel" in step:
            channel = step["channel"]
            if isinstance(channel, bool) or not isinstance(channel, int):
                raise BenchError("%s：steps[%d].channel 必须是整数" % (path, index))
            if channel != relay.CHANNEL_ALL and not 1 <= channel <= 8:
                raise BenchError("%s：steps[%d].channel 越界" % (path, index))


def load_tests(test_dirs):
    tests = {}
    for directory in test_dirs:
        directory = Path(directory)
        if not directory.is_dir():
            continue
        for path in sorted(directory.glob("*.json")):
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except OSError as exc:
                raise BenchError("无法读取测试用例 %s：%s" % (path, exc)) from exc
            except json.JSONDecodeError as exc:
                raise BenchError("测试用例 JSON 无效 %s：%s" % (path, exc)) from exc
            _validate_case(data, path)
            if data["id"] in tests:
                raise BenchError("测试用例 id 重复：%s" % data["id"])
            case = dict(data)
            case["source"] = str(path)
            case["dir"] = str(path.parent)
            case["roles"] = sorted(required_roles(data))
            tests[data["id"]] = case
    return [tests[key] for key in sorted(tests)]


# ---------------------------------------------------------------- 执行器


class RunManager:
    def __init__(self, state_path, test_dirs):
        self.state_path = Path(state_path)
        self.test_dirs = [Path(d) for d in test_dirs]
        self._lock = threading.RLock()
        self._runs = {}
        self._active = None

    def _cases(self):
        return load_tests(self.test_dirs)

    def _find(self, test_id):
        for case in self._cases():
            if case["id"] == test_id:
                return case
        raise BenchError("未知测试用例：%s" % test_id)

    def tests_payload(self):
        state = load_state(self.state_path)
        cases = self._cases()
        out = []
        for case in cases:
            missing = [
                role for role in case["roles"] if not state["roles"].get(role)
            ]
            out.append(
                {
                    "id": case["id"],
                    "name": case["name"],
                    "description": case.get("description", ""),
                    "roles": case["roles"],
                    "steps": [step["type"] for step in case["steps"]],
                    "blocked": case.get("blocked", ""),
                    "missingRoles": missing,
                    "source": case["source"],
                }
            )
        with self._lock:
            active = copy.deepcopy(self._runs.get(self._active)) if self._active else None
        return {
            "ok": True,
            "tests": out,
            "roles": state["roles"],
            "active": active,
        }

    def start(self, test_id):
        case = self._find(test_id)
        if case.get("blocked"):
            raise BenchError("测试用例待 %s，暂不可运行" % case["blocked"])
        state = load_state(self.state_path)
        missing = [role for role in case["roles"] if not state["roles"].get(role)]
        if missing:
            labels = "、".join(ROLE_LABELS[role] for role in missing)
            raise BenchError("缺少资源：%s" % labels)
        with self._lock:
            if self._active and self._runs[self._active]["status"] == "running":
                raise BenchError("已有测试在运行")
            run_id = time.strftime("%Y%m%d-%H%M%S") + "-" + os.urandom(2).hex()
            run = {
                "id": run_id,
                "testId": case["id"],
                "name": case["name"],
                "status": "running",
                "startedAt": time.strftime("%Y-%m-%d %H:%M:%S"),
                "finishedAt": "",
                "steps": [
                    {"index": index, "type": step["type"], "status": "pending", "message": ""}
                    for index, step in enumerate(case["steps"])
                ],
                "logs": [],
                "artifacts": [],
                "error": "",
            }
            self._runs[run_id] = run
            self._active = run_id
            snapshot = copy.deepcopy(run)
        thread = threading.Thread(
            target=self._execute, args=(run_id, case, state), daemon=True
        )
        thread.start()
        return snapshot

    def get_run(self, run_id):
        with self._lock:
            run = self._runs.get(run_id)
            if run is None:
                raise BenchError("未知运行：%s" % run_id)
            return copy.deepcopy(run)

    def _log(self, run_id, message, level="info"):
        with self._lock:
            run = self._runs[run_id]
            run["logs"].append(
                {"time": time.strftime("%H:%M:%S"), "level": level, "message": message}
            )

    def _set_step(self, run_id, index, status, message=""):
        with self._lock:
            self._runs[run_id]["steps"][index].update(
                {"status": status, "message": message}
            )

    def _execute(self, run_id, case, state):
        run_dir = self.state_path.parent / "runs" / run_id
        session = {"relay": None, "baud": None}
        try:
            run_dir.mkdir(parents=True, exist_ok=True)
            self._log(run_id, "运行目录：%s" % run_dir)
            for index, step in enumerate(case["steps"]):
                self._set_step(run_id, index, "running")
                self._log(run_id, "步骤 %d/%d：%s"
                          % (index + 1, len(case["steps"]), step["type"]))
                try:
                    message = self._run_step(
                        run_id, step, case, state, run_dir, session
                    )
                except StepBlocked as exc:
                    self._set_step(run_id, index, "blocked", str(exc))
                    self._finish(run_id, "blocked", str(exc))
                    return
                except BenchError as exc:
                    self._set_step(run_id, index, "failed", str(exc))
                    self._finish(run_id, "failed", str(exc))
                    return
                except Exception as exc:
                    message = "%s: %s" % (type(exc).__name__, exc)
                    self._set_step(run_id, index, "failed", message)
                    self._finish(run_id, "failed", message)
                    return
                self._set_step(run_id, index, "passed", message)
                self._log(run_id, message)
            self._finish(run_id, "passed", "")
        except Exception as exc:  # 线程兜底：任何异常都要落到运行状态
            self._log(run_id, "%s: %s" % (type(exc).__name__, exc), "error")
            self._finish(run_id, "failed", str(exc))
        finally:
            if session["relay"] is not None:
                try:
                    session["relay"].close()
                except Exception:
                    pass

    def _finish(self, run_id, status, error):
        with self._lock:
            run = self._runs[run_id]
            run["status"] = status
            run["error"] = error
            run["finishedAt"] = time.strftime("%Y-%m-%d %H:%M:%S")
        self._log(run_id, "结束：%s" % status,
                  "error" if status == "failed" else "info")

    def _run_step(self, run_id, step, case, state, run_dir, session):
        kind = step["type"]
        if kind == "delay":
            ms = int(step.get("ms", 0))
            time.sleep(ms / 1000.0)
            return "等待 %d ms" % ms
        if kind in ("relay_pulse", "relay_cycle", "relay_set", "relay_query"):
            return self._run_relay_step(run_id, step, state, session)
        if kind == "jlink_reset":
            return self._run_jlink_step(run_id, step, state)
        if kind == "layout_validate":
            path = _resolve_input(step["layout"], Path(case["dir"]))
            layout = _load_layout_file(path)
            return "布局 %s 有效，指纹 0x%04X" % (layout.board, layout.fingerprint)
        if kind == "image_pack":
            return self._run_image_pack(run_id, step, case, run_dir)
        if kind == "image_inspect":
            return self._run_image_inspect(run_id, step, case, run_dir)
        if kind == "serial_download":
            raise StepBlocked("串口下载协议待 OMR-49")
        raise BenchError("未知步骤：%s" % kind)

    def _relay_device(self, state, session):
        if session["relay"] is not None:
            return session["relay"], session["baud"]
        port = state["roles"].get(ROLE_RELAY, "")
        if not port:
            raise BenchError("未分配继电器串口")
        baud = state["settings"]["relayBaud"]
        try:
            session["relay"] = relay.open_relay(port, baud)
            session["baud"] = baud
        except relay.RelayError as exc:
            raise BenchError(str(exc)) from exc
        return session["relay"], session["baud"]

    def _run_relay_step(self, run_id, step, state, session):
        channel = int(step.get("channel", 1))
        dev, baud = self._relay_device(state, session)
        if step["type"] == "relay_pulse":
            ms = int(step.get("ms", 150))
            dev.pulse(channel, ms)
            return "通道 %d 吸合 %d ms 后释放" % (channel, ms)
        if step["type"] == "relay_cycle":
            off_ms = int(step.get("offMs", 1000))
            on_ms = int(step.get("onMs", 300))
            dev.cycle(channel, off_ms, on_ms)
            return "通道 %d 释放 %d ms 后吸合（保持）" % (channel, off_ms)
        if step["type"] == "relay_set":
            value = step.get("state", "off")
            if value == "on":
                dev.on(channel)
                return "通道 %d 吸合" % channel
            if value == "off":
                dev.off(channel)
                return "通道 %d 释放" % channel
            raise BenchError("relay_set.state 必须是 on/off")

        data = dev.query(relay.CHANNEL_ALL, 0.8, relay.default_query_style(baud))
        status = relay.parse_status(data)
        if not status:
            raise BenchError("继电器无应答（检查端口、波特率与型号）")
        message = "继电器状态：" + " ".join(
            "CH%d=%s" % (ch, "ON" if on else "OFF")
            for ch, on in sorted(status.items())
        )
        expect = step.get("expect")
        if "channel" in step and expect in ("on", "off"):
            if status.get(channel) is not (expect == "on"):
                raise BenchError(
                    "CH%d 期望 %s，实际 %s"
                    % (
                        channel,
                        expect.upper(),
                        "ON" if status.get(channel) else "OFF",
                    )
                )
        elif expect == "all_off" and any(status.values()):
            raise BenchError("期望全部释放，实际存在吸合通道")
        elif expect == "all_on" and not all(status.values()):
            raise BenchError("期望全部吸合，实际存在释放通道")
        return message

    def _run_jlink_step(self, run_id, step, state):
        serial = state["roles"].get(ROLE_JLINK, "")
        if not serial:
            raise BenchError("未分配 J-Link")
        exe = find_jlink_exe()
        output = jlink_reset(
            exe,
            serial,
            state["settings"]["jlinkDevice"],
            state["settings"]["jlinkSpeed"],
            step.get("run", True),
        )
        return "J-Link 复位完成\n" + _tail(output, 4)

    def _layout(self, step, case):
        path = _resolve_input(step["layout"], Path(case["dir"]))
        return _load_layout_file(path)

    def _run_image_pack(self, run_id, step, case, run_dir):
        layout = self._layout(step, case)
        slot = step.get("slot", "a")
        slot_part = omimg.layout_slot_by_name(layout, slot)
        if slot_part is None:
            raise BenchError("布局中没有槽 %s" % slot)
        if "payloadFile" in step:
            payload = _resolve_input(step["payloadFile"], Path(case["dir"])).read_bytes()
        else:
            payload = str(step.get("payloadText", "")).encode("utf-8")
        algo = ALGO_MAP.get(step.get("algo", "crc32"))
        if algo is None:
            raise BenchError("未知摘要算法：%s" % step.get("algo"))
        version = omimg.parse_version(step.get("version", "")) if step.get("version") else 0
        image = omimg.build_image(
            payload,
            version=version,
            slot=omimg.SLOT_ID[slot],
            algo=algo,
            flags=omimg.F_SLOT_BOUND,
            payload_offset=layout.payload_offset,
        )
        ok, notes = omimg.verify_image(
            image, slot_capacity=slot_part.size, layout=layout
        )
        if not ok:
            raise BenchError("镜像自检失败：" + "；".join(notes))
        output = _artifact_path(run_dir, step.get("output", "sample_%s.img.bin" % slot))
        output.write_bytes(image)
        with self._lock:
            self._runs[run_id]["artifacts"].append(str(output))
        return "生成 %s（%d 字节，摘要 %s）" % (output.name, len(image), step.get("algo", "crc32"))

    def _run_image_inspect(self, run_id, step, case, run_dir):
        layout = self._layout(step, case)
        path = Path(step["image"])
        if not path.is_absolute():
            path = run_dir / path
            if not path.is_file():
                path = _resolve_input(step["image"], Path(case["dir"]))
        image = path.read_bytes()
        omimg.parse_image(image)
        ok, notes = omimg.verify_image(image, layout=layout)
        if not ok:
            raise BenchError("镜像检查失败：" + "；".join(notes))
        return "镜像检查通过：%s" % "; ".join(notes)
