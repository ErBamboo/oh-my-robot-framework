#!/usr/bin/env python3
"""omhost —— 上位机本地后端。

界面只做展示与交互，全部镜像逻辑复用 omimg（契约的唯一 Python 实现）——
本文件不含任何字段布局或摘要算法，因此不存在"第二份契约实现"可漂移。

用法：
    python tools/omhost/server.py [--port 8787] [--no-browser]
        [--layout <generated.json>] [--layout-source <om_bootcfg.json>]

端点：
    GET  /                      界面
    GET  /api/health            后端进程信息（用于识别旧实例）
    GET  /api/contract          当前契约常量（来自 C 契约头，供界面显示默认值）
    GET  /api/layout            布局源内容 + 校验后的布局投影
    POST /api/layout/validate   请求体 = 布局源 JSON；校验并返回布局投影，不落盘
    POST /api/layout            请求体 = 布局源 JSON；校验通过后原子写回布局源
    GET  /api/resources         探测串口与 J-Link，返回已保存的资源分配
    POST /api/resources         请求体 = {roles, settings}；保存资源分配
    POST /api/resources/relay-probe  请求体 = {port, baud}；只读查询继电器状态
    GET  /api/tests             测试矩阵列表（含阻塞与缺失资源）
    POST /api/tests/run         请求体 = {id}；启动测试
    GET  /api/tests/run?id=     查询测试运行状态与日志
    POST /api/pack              请求体 = 裸负载；查询参数 version/slot/algo/payloadOffset
    POST /api/inspect           请求体 = 镜像；返回头字段与判定顺序的校验结论
"""

import argparse
import base64
import json
import os
import socket
import sys
import time
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "omimg"))

import omimg  # noqa: E402  （路径注入须先于导入）
import omlayout  # noqa: E402  （omimg 已把 tools/omlayout 加入 sys.path）
import bench  # noqa: E402

MAX_BODY = 8 * 1024 * 1024

# 测试台资源与测试矩阵；main() 按命令行参数初始化。
STATE_PATH = bench.default_state_path()
TEST_DIRS = bench.default_test_dirs()
RUN_MANAGER = None
SERVER_STARTED_AT = time.strftime("%Y-%m-%d %H:%M:%S")

# 由 --layout 加载；None 表示只用契约头（单镜像/旧工程）。
LAYOUT = None
LAYOUT_PATH = None
LAYOUT_MTIME = None
# 由 --layout-source 指定；未指定时按已加载布局的板名尝试推断。
LAYOUT_SOURCE_PATH = None


def refresh_layout():
    """构建重新生成清单后按 mtime 自动重载，避免重启后端。"""
    global LAYOUT, LAYOUT_MTIME
    if LAYOUT_PATH is None:
        return
    try:
        mtime = Path(LAYOUT_PATH).stat().st_mtime_ns
    except OSError:
        return
    if mtime == LAYOUT_MTIME:
        return
    try:
        LAYOUT = omimg.load_layout(Path(LAYOUT_PATH))
        LAYOUT_MTIME = mtime
    except omimg.ImageError:
        pass  # 构建写入中可能短暂读到半成品；保留旧清单，下次请求再试


def _layout_info(layout):
    """布局清单的界面投影；layout 为 None 时返回 None。"""
    if layout is None:
        return None
    slots = {}
    for part in layout.partitions:
        if part.role != omlayout.ROLE_APP:
            continue
        slots[part.slot] = {
            "partition": part.name,
            "slotId": part.slot_id,
            "offset": part.offset,
            "size": part.size,
            "linkBase": part.link_base,
            "vtorOffset": part.vtor_offset,
        }
    return {
        "board": layout.board,
        "fingerprint": layout.fingerprint,
        "layoutSha256": layout.layout_sha256,
        "payloadOffset": layout.payload_offset,
        "flash": {"base": layout.flash_base, "size": layout.flash_size},
        "slots": slots,
    }


def layout_info():
    """当前运行布局的界面投影。"""
    return _layout_info(LAYOUT)


def resolve_layout_source_path():
    """解析布局源路径：显式参数优先，否则按板名在工程 cfg 下推断。"""
    if LAYOUT_SOURCE_PATH is not None:
        return Path(LAYOUT_SOURCE_PATH)
    if LAYOUT is None:
        return None
    rel = Path("cfg") / "boards" / LAYOUT.board / "om_bootcfg.json"
    candidates = [Path.cwd() / rel]
    if LAYOUT_PATH is not None:
        base = Path(LAYOUT_PATH).resolve().parent
        candidates.extend(parent / rel for parent in base.parents)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def read_layout_source():
    """读取布局源返回 (path, data)；未配置时 path 为 None。"""
    path = resolve_layout_source_path()
    if path is None:
        return None, None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise omimg.ImageError("无法读取布局源 %s：%s" % (path, exc))
    except json.JSONDecodeError as exc:
        raise omimg.ImageError("布局源 JSON 无效 %s：%s" % (path, exc))
    return path, data


def validate_layout_source(data):
    """校验布局源对象，返回 (layout, info)。"""
    if not isinstance(data, dict):
        raise omimg.ImageError("布局源顶层必须是对象")
    try:
        layout = omlayout.parse_layout(data, "layout source")
    except omlayout.LayoutError as exc:
        raise omimg.ImageError(str(exc))
    return layout, _layout_info(layout)


def layout_source_payload():
    """GET /api/layout 的响应；语义无效时仍返回源内容供界面修复。"""
    try:
        path, data = read_layout_source()
    except omimg.ImageError as exc:
        return {"ok": False, "error": str(exc)}
    if path is None:
        return {"ok": False, "error": "未配置布局源（--layout-source）"}
    try:
        _, info = validate_layout_source(data)
    except omimg.ImageError as exc:
        return {
            "ok": False,
            "error": str(exc),
            "path": str(path),
            "source": data,
        }
    return {"ok": True, "path": str(path), "source": data, "layout": info}


def save_layout_source(data):
    """校验并写回布局源；校验失败不落盘。"""
    layout, info = validate_layout_source(data)
    path = resolve_layout_source_path()
    if path is None:
        raise omimg.ImageError("未配置布局源（--layout-source）")
    text = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temp = path.with_suffix(path.suffix + ".tmp")
        temp.write_text(text, encoding="utf-8", newline="\n")
        os.replace(temp, path)
    except OSError as exc:
        raise omimg.ImageError("写入布局源失败 %s：%s" % (path, exc))
    return {"ok": True, "path": str(path), "layout": info}


def contract_info():
    """界面需要的契约信息——值全部来自 C 契约头，界面不写死任何常量。"""
    refresh_layout()
    payload_offset = LAYOUT.payload_offset if LAYOUT is not None else omimg.PAYLOAD_OFFSET
    return {
        "ok": omimg.CONTRACT_LOADED,
        "message": omimg.CONTRACT_MESSAGE,
        "server": {"pid": os.getpid(), "startedAt": SERVER_STARTED_AT},
        "headerPath": str(omimg._HEADER_PATH),
        "magic": omimg.MAGIC,
        "hdrVersion": omimg.HDR_VERSION,
        "hdrSize": omimg.HDR_SIZE,
        "payloadOffset": payload_offset,
        "digestRegionSize": omimg.DIGEST_REGION_SIZE,
        "slots": omimg.SLOT_ID,
        "layoutPath": str(LAYOUT_PATH) if LAYOUT_PATH is not None else None,
        "layout": layout_info(),
        "algos": [
            {"key": "crc32", "name": "CRC32_ISO_HDLC", "value": omimg.DIGEST_CRC32_ISO_HDLC, "len": 4},
            {"key": "sha256", "name": "SHA256", "value": omimg.DIGEST_SHA256, "len": 32},
            {"key": "none", "name": "NONE", "value": omimg.DIGEST_NONE, "len": 0},
        ],
    }


def pack_response(payload, params):
    """打包并返回头字段 + 校验结论 + 成品镜像（base64）。"""
    refresh_layout()
    version = omimg.parse_version(params.get("version", "")) if params.get("version") else 0
    slot_key = params.get("slot", "a")
    algo_key = params.get("algo", "crc32")
    algo = {"crc32": omimg.DIGEST_CRC32_ISO_HDLC, "sha256": omimg.DIGEST_SHA256, "none": omimg.DIGEST_NONE}[algo_key]
    if params.get("payloadOffset"):
        offset = int(params["payloadOffset"], 0)
    elif LAYOUT is not None:
        offset = LAYOUT.payload_offset
    else:
        offset = omimg.PAYLOAD_OFFSET
    flags = int(params["flags"], 0) if params.get("flags") else omimg.F_SLOT_BOUND

    slot_part = None
    if LAYOUT is not None:
        slot_part = omimg.layout_slot_by_name(LAYOUT, slot_key)
        if slot_part is None:
            raise omimg.ImageError("布局中没有槽 %s" % slot_key)
        if offset != LAYOUT.payload_offset:
            raise omimg.ImageError(
                "负载偏移与布局不一致：参数 0x%X，布局 0x%X"
                % (offset, LAYOUT.payload_offset)
            )

    image = omimg.build_image(
        payload,
        version=version,
        slot=omimg.SLOT_ID[slot_key],
        algo=algo,
        flags=flags,
        payload_offset=offset,
    )
    hdr = omimg.parse_image(image)
    ok, notes = omimg.verify_image(
        image,
        slot_capacity=slot_part.size if slot_part is not None else None,
        layout=LAYOUT,
    )
    return _result(image, hdr, ok, notes, extra={"imageBase64": base64.b64encode(image).decode("ascii")})


def inspect_response(image):
    """解析已有镜像并给出判定顺序的结论。"""
    refresh_layout()
    try:
        hdr = omimg.parse_image(image)
    except omimg.ImageError as exc:
        return {"ok": False, "kind": "inspect", "fatal": str(exc), "fields": [], "verdict": {"ok": False, "notes": [str(exc)]}}
    ok, notes = omimg.verify_image(image, layout=LAYOUT)
    return _result(image, hdr, ok, notes, extra={})


def _result(image, hdr, ok, notes, extra):
    digest_hex = ""
    if hdr["digestLen"] and hdr["digestOffset"] + hdr["digestLen"] <= len(image):
        digest_hex = image[hdr["digestOffset"] : hdr["digestOffset"] + hdr["digestLen"]].hex()
    result = {
        "ok": True,
        "fields": omimg.header_rows(hdr),
        "verdict": {"ok": bool(ok), "notes": list(notes)},
        "digestHex": digest_hex,
        "span": {
            "header": omimg.HDR_SIZE,
            "payloadOffset": hdr["payloadOffset"],
            "imageSize": hdr["imageSize"],
            "digestOffset": hdr["digestOffset"],
            "digestRegionSize": hdr["digestRegionSize"],
            "total": hdr["imageTotalSize"],
        },
        "layout": layout_info(),
    }
    result.update(extra)
    return result


class Handler(BaseHTTPRequestHandler):
    server_version = "omhost"

    def log_message(self, fmt, *args):  # 静默：界面自带状态反馈，控制台不刷日志
        pass

    # -- 工具 ---------------------------------------------------------------

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > MAX_BODY:
            raise ValueError("请求体长度非法：%d" % length)
        return self.rfile.read(length)

    # -- 路由 ---------------------------------------------------------------

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        params = {k: v[0] for k, v in parse_qs(parsed.query).items()}
        if path in ("/", "/index.html"):
            page = (HERE / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)
        elif path == "/api/health":
            self._json({"ok": True, "pid": os.getpid(), "startedAt": SERVER_STARTED_AT})
        elif path == "/api/contract":
            self._json(contract_info())
        elif path == "/api/layout":
            self._json(layout_source_payload())
        elif path == "/api/resources":
            try:
                self._json(bench.resources_payload(STATE_PATH))
            except bench.BenchError as exc:
                self._json({"ok": False, "error": str(exc)})
        elif path == "/api/tests":
            try:
                self._json(self._manager().tests_payload())
            except bench.BenchError as exc:
                self._json({"ok": False, "error": str(exc)})
        elif path == "/api/tests/run":
            try:
                run_id = params.get("id", "")
                self._json({"ok": True, "run": self._manager().get_run(run_id)})
            except bench.BenchError as exc:
                self._json({"ok": False, "error": str(exc)})
        else:
            self._json({"error": "未知路径 %s" % path}, 404)

    def _manager(self):
        if RUN_MANAGER is None:
            raise bench.BenchError("测试台未初始化")
        return RUN_MANAGER

    def _json_source(self, body):
        try:
            return json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise omimg.ImageError("请求体不是有效 JSON：%s" % exc)

    def do_POST(self):
        parsed = urlparse(self.path)
        params = {k: v[0] for k, v in parse_qs(parsed.query).items()}
        try:
            body = self._body()
        except ValueError as exc:
            self._json({"ok": False, "error": str(exc)}, 400)
            return

        try:
            if parsed.path == "/api/pack":
                self._json(pack_response(body, params))
            elif parsed.path == "/api/inspect":
                self._json(inspect_response(body))
            elif parsed.path == "/api/layout/validate":
                _, info = validate_layout_source(self._json_source(body))
                self._json({"ok": True, "layout": info})
            elif parsed.path == "/api/layout":
                self._json(save_layout_source(self._json_source(body)))
            elif parsed.path == "/api/resources":
                state = bench.save_state(STATE_PATH, self._json_source(body))
                self._json({"ok": True, "state": state})
            elif parsed.path == "/api/resources/relay-probe":
                data = self._json_source(body)
                port = data.get("port", "")
                baud = int(data.get("baud", 9600))
                self._json(bench.probe_relay(port, baud))
            elif parsed.path == "/api/tests/run":
                data = self._json_source(body)
                run = self._manager().start(str(data.get("id", "")))
                self._json({"ok": True, "run": run})
            else:
                self._json({"error": "未知路径 %s" % parsed.path}, 404)
        except omimg.ImageError as exc:
            self._json({"ok": False, "error": str(exc)}, 200)
        except bench.BenchError as exc:
            self._json({"ok": False, "error": str(exc)}, 200)
        except Exception as exc:  # 兜底：界面要能看到失败原因，而不是连接断掉
            self._json({"ok": False, "error": "%s: %s" % (type(exc).__name__, exc)}, 200)


def port_in_use(host, port):
    """端口上已有监听者时返回 True（Windows 的 SO_REUSEADDR 会允许重复绑定）。"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.3)
        return sock.connect_ex((host, port)) == 0


def existing_omhost(host, port):
    """端口上是本程序实例时返回 /api/health 内容，否则返回 None。"""
    try:
        with urllib.request.urlopen(
            "http://%s:%d/api/health" % (host, port), timeout=0.6
        ) as response:
            data = json.load(response)
    except Exception:  # 探测失败一律视为非本程序；随后按端口占用报错
        return None
    return data if data.get("ok") else None


def main(argv=None):
    global LAYOUT, LAYOUT_PATH, LAYOUT_MTIME, LAYOUT_SOURCE_PATH
    global STATE_PATH, TEST_DIRS, RUN_MANAGER

    # Windows 控制台默认代码页不保证能编码中文，统一切到 UTF-8 并容错
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(encoding="utf-8", errors="replace")
            except (ValueError, OSError):
                pass

    parser = argparse.ArgumentParser(description="上位机本地后端（镜像打包与检查）")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    parser.add_argument("--layout", default=None, help="om_bootcfg.generated.json（布局清单）")
    parser.add_argument(
        "--layout-source", default=None, help="om_bootcfg.json（布局源，供界面编辑）"
    )
    parser.add_argument(
        "--state", default=None, help="资源分配文件（默认 ~/.omhost/config.json）"
    )
    parser.add_argument(
        "--test-dir", action="append", default=[],
        help="额外的测试用例目录（可重复；每目录一个用例一个 JSON）",
    )
    args = parser.parse_args(argv)

    if args.layout_source is not None:
        LAYOUT_SOURCE_PATH = args.layout_source
    STATE_PATH = Path(args.state) if args.state else bench.default_state_path()
    TEST_DIRS = list(
        dict.fromkeys(bench.default_test_dirs() + [Path(p) for p in args.test_dir])
    )
    RUN_MANAGER = bench.RunManager(STATE_PATH, TEST_DIRS)

    ok, message = omimg.load_contract()
    print("契约头：%s" % omimg._HEADER_PATH)
    print("加载：%s" % ("成功" if ok else message))
    if args.layout:
        try:
            LAYOUT = omimg.load_layout(args.layout)
        except omimg.ImageError as exc:
            print("布局：加载失败：%s" % exc, file=sys.stderr)
            return 1
        LAYOUT_PATH = args.layout
        try:
            LAYOUT_MTIME = Path(LAYOUT_PATH).stat().st_mtime_ns
        except OSError:
            LAYOUT_MTIME = None
        print("布局：%s（%s，指纹 0x%04X）" % (LAYOUT.board, args.layout, LAYOUT.fingerprint))
    else:
        print("布局：未指定（只用契约头）")
    source_path = resolve_layout_source_path()
    if source_path is not None:
        print("布局源：%s（界面可编辑）" % source_path)
    else:
        print("布局源：未找到（--layout-source 未指定且 cfg 下无对应文件）")
    print("测试台：资源分配 %s" % STATE_PATH)
    print("测试台：用例目录 %s" % "、".join(str(path) for path in TEST_DIRS))
    print("魔数 0x%08X / 头长 %d / 负载偏移 0x%X / 摘要区 %d"
          % (omimg.MAGIC, omimg.HDR_SIZE,
             LAYOUT.payload_offset if LAYOUT is not None else omimg.PAYLOAD_OFFSET,
             omimg.DIGEST_REGION_SIZE))

    host = "127.0.0.1"
    url = "http://%s:%d/" % (host, args.port)
    if port_in_use(host, args.port):
        running = existing_omhost(host, args.port)
        if running is not None:
            print(
                "omhost 已在运行：%s（PID %s，启动于 %s）"
                % (url, running.get("pid", "?"), running.get("startedAt", "?"))
            )
            if not args.no_browser:
                webbrowser.open(url)
            return 0
        print(
            "错误：端口 %d 已被其它程序占用。\n"
            "请先结束占用进程（Windows: netstat -ano | findstr :%d 后 taskkill /PID <pid> /F），"
            "或改用 --port。" % (args.port, args.port),
            file=sys.stderr,
        )
        return 1

    print("界面：%s（Ctrl+C 退出）" % url)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        ThreadingHTTPServer((host, args.port), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\n已退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
