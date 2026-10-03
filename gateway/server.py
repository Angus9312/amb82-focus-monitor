#!/usr/bin/env python3
"""AMB82-MINI 本機影像閘道：不需額外 Python 套件，影像轉換需 ffmpeg。"""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import pathlib
import subprocess
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "dist"


class GatewayState:
    def __init__(self, board_ip: str, rtsp_port: int, status_port: int):
        self.board_ip = board_ip
        self.rtsp_port = rtsp_port
        self.status_port = status_port
        self.lock = threading.Lock()

    def snapshot(self):
        with self.lock:
            return self.board_ip, self.rtsp_port, self.status_port

    def update(self, board_ip: str, rtsp_port: int, status_port: int):
        with self.lock:
            self.board_ip = board_ip.strip()
            self.rtsp_port = int(rtsp_port)
            self.status_port = int(status_port)


STATE: GatewayState


class Handler(BaseHTTPRequestHandler):
    server_version = "Focus82Gateway/1.0"

    def log_message(self, fmt, *args):
        print(f"[{self.log_date_time_string()}] {fmt % args}")

    def _json(self, payload, status=200):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/api/config":
            board_ip, rtsp_port, status_port = STATE.snapshot()
            return self._json({"boardIp": board_ip, "rtspPort": rtsp_port, "statusPort": status_port})
        if path == "/api/status":
            return self.proxy_status()
        if path == "/stream":
            return self.stream_rtsp()
        self.serve_file(path)

    def do_POST(self):
        if self.path.split("?", 1)[0] != "/api/config":
            return self._json({"error": "not found"}, 404)
        try:
            length = int(self.headers.get("Content-Length", "0"))
            data = json.loads(self.rfile.read(length))
            board_ip = str(data.get("boardIp", "")).strip()
            rtsp_port = int(data.get("rtspPort", 554))
            status_port = int(data.get("statusPort", 8080))
            if not board_ip or not (1 <= rtsp_port <= 65535) or not (1 <= status_port <= 65535):
                raise ValueError("invalid configuration")
            STATE.update(board_ip, rtsp_port, status_port)
            self._json({"ok": True, "boardIp": board_ip, "rtspPort": rtsp_port, "statusPort": status_port})
        except (ValueError, TypeError, json.JSONDecodeError):
            self._json({"error": "請確認 IP 與連接埠格式"}, 400)

    def serve_file(self, request_path: str):
        relative = "index.html" if request_path in ("", "/") else request_path.lstrip("/")
        target = (WEB_ROOT / relative).resolve()
        if WEB_ROOT.resolve() not in target.parents and target != WEB_ROOT.resolve():
            self.send_error(403)
            return
        if not target.is_file():
            self.send_error(404)
            return
        body = target.read_bytes()
        content_type = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def proxy_status(self):
        board_ip, _, status_port = STATE.snapshot()
        if not board_ip:
            return self._json({"error": "尚未設定 AMB82-MINI IP"}, 503)
        started = time.perf_counter()
        try:
            with urllib.request.urlopen(f"http://{board_ip}:{status_port}/status", timeout=0.8) as response:
                payload = json.loads(response.read())
            payload["latencyMs"] = round((time.perf_counter() - started) * 1000)
            self._json(payload)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as exc:
            self._json({"error": "無法讀取開發板狀態", "detail": str(exc)}, 503)

    def stream_rtsp(self):
        board_ip, rtsp_port, _ = STATE.snapshot()
        if not board_ip:
            self.send_error(503, "Board IP is not configured")
            return
        command = [
            "ffmpeg", "-loglevel", "error", "-rtsp_transport", "tcp",
            "-i", f"rtsp://{board_ip}:{rtsp_port}", "-an", "-vf", "fps=10,scale=960:-2",
            "-q:v", "6", "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1"
        ]
        try:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
        except FileNotFoundError:
            self.send_error(503, "ffmpeg is not installed")
            return
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        buffer = bytearray()
        try:
            while process.poll() is None:
                chunk = process.stdout.read(8192)
                if not chunk:
                    break
                buffer.extend(chunk)
                while True:
                    start = buffer.find(b"\xff\xd8")
                    end = buffer.find(b"\xff\xd9", start + 2) if start >= 0 else -1
                    if start < 0 or end < 0:
                        if len(buffer) > 2_000_000:
                            del buffer[:-2]
                        break
                    frame = bytes(buffer[start:end + 2])
                    del buffer[:end + 2]
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n")
                    self.wfile.write(f"Content-Length: {len(frame)}\r\n\r\n".encode())
                    self.wfile.write(frame + b"\r\n")
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            process.terminate()
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()


def main():
    parser = argparse.ArgumentParser(description="AMB82-MINI 專注偵測本機閘道")
    parser.add_argument("--board-ip", default=os.environ.get("AMB82_IP", ""), help="AMB82-MINI 的區域網路 IP")
    parser.add_argument("--port", type=int, default=8000, help="網頁連接埠（預設 8000）")
    parser.add_argument("--rtsp-port", type=int, default=554)
    parser.add_argument("--status-port", type=int, default=8080)
    args = parser.parse_args()
    global STATE
    STATE = GatewayState(args.board_ip, args.rtsp_port, args.status_port)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"專注偵測面板：http://127.0.0.1:{args.port}")
    if not args.board_ip:
        print("尚未設定 AMB82-MINI IP，可在網頁右上角設定。")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止。")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
