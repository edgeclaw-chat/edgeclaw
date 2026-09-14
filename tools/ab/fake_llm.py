#!/usr/bin/env python3
"""假 LLM：本地自签 TLS + 固定响应序列。

见 README §"为什么能覆盖 LLM 回路"。关键：客户端在未配 cacert 时
authmode = VERIFY_NONE（edc_https_client.c:717-730），所以自签证书可直接用。

额外收益：**记录每个请求体**。请求体就是装配出来的镜像（prompt），
比对它比比对 session.json 更直接地验证"镜像装配没变"。

用法：
    fake_llm.py --port 8443 --responses responses/smoke.json \
                --request-log /tmp/ab/out/a/llm_requests.jsonl
"""
import argparse
import json
import os
import ssl
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RESPONSES = []
REQUEST_LOG = None
COUNTER = 0
LOCK = threading.Lock()


def ensure_cert(dirpath):
    crt = os.path.join(dirpath, "cert.pem")
    key = os.path.join(dirpath, "key.pem")
    if os.path.exists(crt) and os.path.exists(key):
        return crt, key
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", key, "-out", crt, "-days", "3650",
         "-subj", "/CN=127.0.0.1",
         "-addext", "subjectAltName=IP:127.0.0.1"],
        check=True, capture_output=True,
    )
    return crt, key


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # 静音，避免污染 stderr
        pass

    def do_POST(self):
        global COUNTER
        n = int(self.headers.get("Content-Length", 0) or 0)
        raw = self.rfile.read(n) if n else b""

        with LOCK:
            idx = COUNTER
            COUNTER += 1
        # 用完后重复最后一个 —— 保证确定性（README §假 LLM 响应序列）
        resp = RESPONSES[min(idx, len(RESPONSES) - 1)] if RESPONSES else {}

        if REQUEST_LOG:
            try:
                body = json.loads(raw.decode("utf-8", "replace"))
            except Exception:
                body = raw.decode("utf-8", "replace")
            with open(REQUEST_LOG, "a", encoding="utf-8") as f:
                f.write(json.dumps({"index": idx, "path": self.path,
                                    "body": body}, ensure_ascii=False,
                                   sort_keys=True) + "\n")

        # `__delay`: 应答前睡这么久。作用是在客户端**在途**时制造一个窗口，
        # 好让脚本在那个窗口里取消（否则立即应答就没有可取消的东西）。
        # 请求先记盘再睡 —— 即使客户端随后断连，请求记录也留得下。
        # 该键是夹具私有的，不发给客户端。
        resp = dict(resp)
        delay = resp.pop("__delay", 0)
        if delay:
            time.sleep(delay)

        data = json.dumps(resp).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError, OSError):
            # 客户端已经取消并关掉了连接 —— 这正是取消场景要覆盖的情形，
            # 不该让服务器崩掉。应答被丢弃 = "迟到事件必须无害" 的上游一半。
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--responses", required=True)
    ap.add_argument("--request-log", default=None)
    ap.add_argument("--cert-dir", default="/tmp/ab/cert")
    args = ap.parse_args()

    global RESPONSES, REQUEST_LOG
    with open(args.responses, encoding="utf-8") as f:
        RESPONSES = json.load(f)
    REQUEST_LOG = args.request_log
    if REQUEST_LOG:
        os.makedirs(os.path.dirname(REQUEST_LOG), exist_ok=True)
        open(REQUEST_LOG, "w").close()

    os.makedirs(args.cert_dir, exist_ok=True)
    crt, key = ensure_cert(args.cert_dir)

    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    sys.stderr.write("fake_llm: listening on 127.0.0.1:%d (%d responses)\n"
                     % (args.port, len(RESPONSES)))
    sys.stderr.flush()
    httpd.serve_forever()


if __name__ == "__main__":
    main()
