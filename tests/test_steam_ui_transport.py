import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def frame(data, opcode=1, final=True):
    size = len(data)
    header = bytes([(0x80 if final else 0) | opcode])
    if size < 126:
        return header + bytes([size]) + data
    if size <= 65535:
        return header + b"\x7e" + struct.pack("!H", size) + data
    return header + b"\x7f" + struct.pack("!Q", size) + data


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == "/json/list":
            body = json.dumps(
                [
                    {
                        "title": "SharedJSContext",
                        "url": "https://steamloopback.host/shared",
                        "webSocketDebuggerUrl": self.server.target,
                    }
                ]
            ).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        key = self.headers["Sec-WebSocket-Key"]
        self.close_connection = True
        if self.server.mode == "handshake_timeout":
            time.sleep(0.8)
            return
        accept = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header(
            "Sec-WebSocket-Accept", "wrong" if self.server.mode == "handshake" else accept
        )
        self.end_headers()
        self.connection.settimeout(2)
        message = bytearray()
        try:
            while True:
                header = self.rfile.read(2)
                if len(header) != 2:
                    return
                first, second = header
                assert second & 0x80
                size = second & 127
                if size == 126:
                    size = struct.unpack("!H", self.rfile.read(2))[0]
                elif size == 127:
                    size = struct.unpack("!Q", self.rfile.read(8))[0]
                assert size <= 1024 * 1024
                mask = self.rfile.read(4)
                data = self.rfile.read(size)
                data = bytes(value ^ mask[i % 4] for i, value in enumerate(data))
                if first & 15 == 10:
                    self.server.pongs += 1
                    continue
                if first & 15 not in (0, 1):
                    return
                if first & 15 == 1:
                    message.clear()
                message.extend(data)
                assert len(message) <= 1024 * 1024
                if not first & 0x80:
                    continue
                request = json.loads(message)
                assert request["method"] == "Runtime.evaluate"
                self.server.requests += 1
                if self.server.mode == "timeout":
                    time.sleep(0.6)
                    return
                if self.server.mode == "close":
                    self.wfile.write(frame(b"", 8))
                    self.rfile.read(2)
                    return
                if self.server.mode == "oversized":
                    self.wfile.write(b"\x81\x7f" + struct.pack("!Q", 1024 * 1024 + 1))
                    self.rfile.read(2)
                    return
                value = {"active": True, "serial": "TEST1234", "index": 3}
                if self.server.mode == "large":
                    value["padding"] = "x" * 70000
                reply = json.dumps(
                    {"id": request["id"], "result": {"result": {"value": value}}}
                ).encode()
                if self.server.mode == "binary":
                    self.wfile.write(frame(reply, 2))
                elif self.server.mode == "fragmented":
                    self.wfile.write(
                        frame(b'{"method":"notification"}')
                        + frame(reply[:20], final=False)
                        + frame(b"ping", 9)
                        + frame(reply[20:], 0)
                    )
                else:
                    self.wfile.write(frame(reply))
        except BrokenPipeError, ConnectionResetError, socket.timeout:
            pass


class SteamUiTransportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 8080), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        self.server.mode = "normal"
        self.server.target = "ws://127.0.0.1:8080/devtools/page/TEST1234"
        self.server.requests = self.server.pongs = 0

    def run_transport(self, **values):
        result = subprocess.run(
            [os.environ.get("CONTROLLER_UTILS_REPLAY", str(ROOT / "build/native_replay"))],
            input=json.dumps(
                {"component": "steam_ui_transport", "connections": 1, "requests": 1, **values}
            )
            + "\n",
            capture_output=True,
            text=True,
            check=True,
            timeout=10,
        )
        return json.loads(result.stdout)

    def test_connections_and_repeated_requests_release_all_descriptors(self):
        result = self.run_transport(connections=10, requests=3)
        self.assertNotIn("error", result)
        self.assertEqual(len(result["replies"]), 30)
        self.assertEqual(self.server.requests, 30)
        self.assertEqual(result["fds_before"], result["fds_after"])

    def test_fragmentation_notifications_ping_and_large_frames(self):
        for mode in ("fragmented", "large"):
            with self.subTest(mode=mode):
                self.server.mode = mode
                result = self.run_transport(requests=2)
                self.assertNotIn("error", result)
                self.assertEqual(result["replies"][-1]["serial"], "TEST1234")
                if mode == "fragmented":
                    self.assertGreaterEqual(self.server.pongs, 1)

    def test_bad_handshake_binary_close_and_oversized_frames_are_rejected(self):
        for mode, error in (
            ("handshake", "Sec-WebSocket-Accept"),
            ("binary", "Invalid Steam debugger websocket message"),
            ("close", "closed"),
            ("oversized", "message exceeded"),
        ):
            with self.subTest(mode=mode):
                self.server.mode = mode
                self.assertIn(error, self.run_transport()["error"])

    def test_stalled_response_is_bounded(self):
        for mode in ("timeout", "handshake_timeout"):
            with self.subTest(mode=mode):
                self.server.mode = mode
                start = time.monotonic()
                self.assertIn("timed out", self.run_transport()["error"])
                self.assertLess(time.monotonic() - start, 1.5)

    def test_discovery_refuses_external_targets(self):
        self.server.target = "ws://example.com:8080/devtools/page/TEST1234"
        self.assertIn("expected loopback", self.run_transport()["error"])
        self.assertEqual(self.server.requests, 0)
