#!/usr/bin/env python3
"""omhost —— 上位机本地后端。

界面只做展示与交互，全部镜像逻辑复用 omimg（契约的唯一 Python 实现）——
本文件不含任何字段布局或摘要算法，因此不存在"第二份契约实现"可漂移。

用法：
    python tools/omhost/server.py [--port 8787] [--no-browser]

端点：
    GET  /              界面
    GET  /api/contract  当前契约常量（来自 C 契约头，供界面显示默认值）
    POST /api/pack      请求体 = 裸负载；查询参数 version/slot/algo/payloadOffset
    POST /api/inspect   请求体 = 镜像；返回头字段与判定顺序的校验结论
"""

import argparse
import base64
import json
import sys
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "omimg"))

import omimg  # noqa: E402  （路径注入须先于导入）

MAX_BODY = 8 * 1024 * 1024


def contract_info():
    """界面需要的契约信息——值全部来自 C 契约头，界面不写死任何常量。"""
    return {
        "ok": omimg.CONTRACT_LOADED,
        "message": omimg.CONTRACT_MESSAGE,
        "headerPath": str(omimg._HEADER_PATH),
        "magic": omimg.MAGIC,
        "hdrVersion": omimg.HDR_VERSION,
        "hdrSize": omimg.HDR_SIZE,
        "payloadOffset": omimg.PAYLOAD_OFFSET,
        "digestRegionSize": omimg.DIGEST_REGION_SIZE,
        "slots": omimg.SLOT_ID,
        "algos": [
            {"key": "crc32", "name": "CRC32_ISO_HDLC", "value": omimg.DIGEST_CRC32_ISO_HDLC, "len": 4},
            {"key": "sha256", "name": "SHA256", "value": omimg.DIGEST_SHA256, "len": 32},
            {"key": "none", "name": "NONE", "value": omimg.DIGEST_NONE, "len": 0},
        ],
    }


def pack_response(payload, params):
    """打包并返回头字段 + 校验结论 + 成品镜像（base64）。"""
    version = omimg.parse_version(params.get("version", "")) if params.get("version") else 0
    slot_key = params.get("slot", "a")
    algo_key = params.get("algo", "crc32")
    algo = {"crc32": omimg.DIGEST_CRC32_ISO_HDLC, "sha256": omimg.DIGEST_SHA256, "none": omimg.DIGEST_NONE}[algo_key]
    offset = int(params.get("payloadOffset") or omimg.PAYLOAD_OFFSET, 0)
    flags = int(params["flags"], 0) if params.get("flags") else omimg.F_SLOT_BOUND

    image = omimg.build_image(
        payload,
        version=version,
        slot=omimg.SLOT_ID[slot_key],
        algo=algo,
        flags=flags,
        payload_offset=offset,
    )
    hdr = omimg.parse_image(image)
    ok, notes = omimg.verify_image(image)
    return _result(image, hdr, ok, notes, extra={"imageBase64": base64.b64encode(image).decode("ascii")})


def inspect_response(image):
    """解析已有镜像并给出判定顺序的结论。"""
    try:
        hdr = omimg.parse_image(image)
    except omimg.ImageError as exc:
        return {"ok": False, "kind": "inspect", "fatal": str(exc), "fields": [], "verdict": {"ok": False, "notes": [str(exc)]}}
    ok, notes = omimg.verify_image(image)
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
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            page = (HERE / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)
        elif path == "/api/contract":
            self._json(contract_info())
        else:
            self._json({"error": "未知路径 %s" % path}, 404)

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
            else:
                self._json({"error": "未知路径 %s" % parsed.path}, 404)
        except omimg.ImageError as exc:
            self._json({"ok": False, "error": str(exc)}, 200)
        except Exception as exc:  # 兜底：界面要能看到失败原因，而不是连接断掉
            self._json({"ok": False, "error": "%s: %s" % (type(exc).__name__, exc)}, 200)


def main(argv=None):
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
    args = parser.parse_args(argv)

    ok, message = omimg.load_contract()
    print("契约头：%s" % omimg._HEADER_PATH)
    print("加载：%s" % ("成功" if ok else message))
    print("魔数 0x%08X / 头长 %d / 负载偏移 0x%X / 摘要区 %d"
          % (omimg.MAGIC, omimg.HDR_SIZE, omimg.PAYLOAD_OFFSET, omimg.DIGEST_REGION_SIZE))

    url = "http://127.0.0.1:%d/" % args.port
    print("界面：%s（Ctrl+C 退出）" % url)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\n已退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
