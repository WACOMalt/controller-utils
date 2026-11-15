import ctypes
import fcntl
import hashlib
import http.server
import json
import os
import ssl
import subprocess
import threading
import time
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import tls_fixture

ROOT = Path(__file__).resolve().parents[1]
BODY = b"controller-utils-download-fixture"


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def handle_error(self, _request, _address):
        pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        self.server.requests.append((self.path, self.headers.get("User-Agent")))
        try:
            self.respond()
        except OSError:
            pass

    def respond(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", self.server.base + "/ok")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path in {"/loop", "/downgrade", "/foreign", "/credentials"}:
            self.send_response(302)
            self.send_header(
                "Location",
                {
                    "/loop": self.server.base + "/loop",
                    "/downgrade": self.server.base.replace("https:", "http:") + "/ok",
                    "/foreign": "https://example.com/ok",
                    "/credentials": self.server.base.replace("https://", "https://user:secret@")
                    + "/ok",
                }[self.path],
            )
            self.send_header("Content-Length", "3")
            self.end_headers()
            self.wfile.write(b"abc")
            return
        if self.path == "/not-found":
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Connection", "close")
        if self.path in ("/chunked", "/chunked-large", "/trickle"):
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            chunks = [b"abc", b"def"]
            if self.path == "/chunked-large":
                chunks = [b"12345678"] * 2
            if self.path == "/trickle":
                chunks = [b"x"] * 200
            for chunk in chunks:
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                self.wfile.flush()
                if self.path == "/trickle":
                    time.sleep(0.02)
            self.wfile.write(b"0\r\n\r\n")
            return
        if self.path == "/stream-large":
            self.end_headers()
            self.wfile.write(b"x" * 4096)
            return
        if self.path == "/large":
            self.send_header("Content-Length", "4096")
            self.end_headers()
            return
        if self.path == "/bad-length":
            self.send_header("Content-Length", "invalid")
            self.end_headers()
            return
        if self.path == "/conflicting-length":
            self.send_header("Content-Length", "3")
            self.send_header("Content-Length", "4")
            self.end_headers()
            self.wfile.write(b"abc")
            return
        if self.path == "/long-header":
            self.send_header("X-Fixture", "x" * 65540)
        if self.path == "/many-headers":
            for index in range(100):
                self.send_header(f"X-Fixture-{index}", "x" * 800)
        if self.path == "/short":
            self.send_header("Content-Length", "20")
            self.end_headers()
            self.wfile.write(b"abc")
            return
        if self.path == "/hold":
            self.send_header("Content-Length", "3")
            self.end_headers()
            self.server.held.set()
            time.sleep(4)
            self.wfile.write(b"abc")
            return
        body = b"" if self.path == "/empty" else BODY
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class NativeDownloadTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = TemporaryDirectory(prefix="native-download-tls-", dir=ROOT / "build")
        cls.work = Path(cls.directory.name)
        cls.certificate, key = tls_fixture.certificate(cls.work)
        cls.server = Server(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cls.certificate, key)
        cls.server.socket = context.wrap_socket(
            cls.server.socket, server_side=True, do_handshake_on_connect=False
        )
        cls.server.base = f"https://127.0.0.1:{cls.server.server_port}"
        cls.server.requests = []
        cls.server.held = threading.Event()
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.executable = os.environ.get(
            "CONTROLLER_UTILS_REPLAY", str(ROOT / "build/native_replay")
        )
        cls.environment = {**os.environ, "NO_PROXY": "*", "no_proxy": "*"}

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.directory.cleanup()

    def setUp(self):
        self.directory = TemporaryDirectory(prefix="case-", dir=self.work)
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name) / "download"
        self.server.held.clear()

    def request(self, path="/ok", **values):
        return {
            "component": "download",
            "url": self.server.base + path,
            "destination": str(self.output),
            "ca": str(self.certificate),
            "limit": 65536,
            "mode": "normal",
            **values,
        }

    def run_requests(self, requests):
        result = subprocess.run(
            [self.executable],
            input="".join(json.dumps(request) + "\n" for request in requests),
            capture_output=True,
            text=True,
            timeout=30,
            check=True,
            env=self.environment,
        )
        return [json.loads(line) for line in result.stdout.splitlines()]

    def run_one(self, path="/ok", **values):
        return self.run_requests([self.request(path, **values)])[0]

    def test_success_and_one_hundred_clean_lifecycles(self):
        requests = [
            self.request(destination=str(self.output.with_name(f"download-{i}")))
            for i in range(100)
        ]
        results = self.run_requests(requests)
        self.assertEqual(len(results), 100)
        for request, result in zip(requests, results):
            self.assertNotIn("error", result)
            self.assertEqual(result["bytes"], len(BODY))
            self.assertEqual(result["sha256"], hashlib.sha256(BODY).hexdigest())
            self.assertEqual(Path(request["destination"]).read_bytes(), BODY)
            self.assertEqual(Path(request["destination"]).stat().st_mode & 0o777, 0o600)
        self.assertEqual(len({result["fds_after"] for result in results[10:]}), 1)
        self.assertEqual(results[-1]["fds_before"], results[-1]["fds_after"])
        self.assertEqual(self.server.requests[-1][1], "Controller-Utils-Updater/1")

    def test_chunked_and_empty_bodies(self):
        result = self.run_one("/chunked")
        self.assertEqual(result["bytes"], 6)
        self.assertEqual(self.output.read_bytes(), b"abcdef")
        self.output.unlink()
        result = self.run_one("/empty", limit=0)
        self.assertEqual(result["bytes"], 0)

    def test_same_origin_redirect_is_followed(self):
        start = len(self.server.requests)
        result = self.run_one("/redirect")
        self.assertNotIn("error", result)
        self.assertEqual([path for path, _ in self.server.requests[start:]], ["/redirect", "/ok"])
        self.assertEqual(self.output.read_bytes(), BODY)

    def test_redirect_boundaries_preserve_no_partial_file(self):
        for path in ("/loop", "/downgrade", "/foreign", "/credentials"):
            with self.subTest(path=path):
                self.assertIn("error", self.run_one(path))
                self.assertFalse(self.output.exists())

    def test_untrusted_certificate_and_wrong_hostname_fail(self):
        self.assertIn("error", self.run_one(ca=""))
        self.assertFalse(self.output.exists())
        url = self.server.base.replace("127.0.0.1", "localhost") + "/ok"
        self.assertIn("error", self.run_one(url=url))
        self.assertFalse(self.output.exists())

    def test_declared_streamed_and_chunked_limits(self):
        for path in ("/large", "/stream-large", "/chunked-large"):
            with self.subTest(path=path):
                result = self.run_one(path, limit=10)
                self.assertIn("size limit", result["error"])
                self.assertFalse(self.output.exists())

    def test_truncated_invalid_and_error_responses(self):
        for path in ("/short", "/bad-length", "/conflicting-length", "/long-header", "/not-found"):
            with self.subTest(path=path):
                self.assertIn("error", self.run_one(path))
                self.assertFalse(self.output.exists())

    def test_cancel_and_total_deadline_interrupt_slow_responses(self):
        for path, mode in (("/hold", "cancel"), ("/trickle", "deadline")):
            with self.subTest(mode=mode):
                result = self.run_one(path, mode=mode)
                self.assertEqual(result["kind"], "cancelled")
                self.assertLess(result["elapsed"], 2)
                self.assertFalse(self.output.exists())

    def test_idle_deadline(self):
        result = self.run_one("/hold")
        self.assertEqual(result["kind"], "timeout")
        self.assertGreaterEqual(result["elapsed"], 2.7)
        self.assertLess(result["elapsed"], 4.5)
        self.assertFalse(self.output.exists())

    def test_cancel_during_delayed_dns_lookup(self):
        result, marker = self.delayed_dns("cancel_dns")
        self.assertEqual(result["kind"], "cancelled")
        self.assertLess(result["elapsed"], 1)
        self.assertFalse(Path(f"/proc/{int(marker.read_text())}").exists())

    def dns_environment(self, **environment):
        marker = self.output.with_name("dns-started")
        preload = str(ROOT / "build/fake_dns.so")
        if "sanitize" in Path(self.executable).parts:
            asan = subprocess.check_output(
                ["g++", "-print-file-name=libasan.so"], text=True
            ).strip()
            preload = asan + ":" + preload
        return {
            **self.environment,
            "LD_PRELOAD": preload,
            "CT_DNS_TEST_STARTED": str(marker),
            **environment,
        }, marker

    def delayed_dns(self, mode="normal", pass_fds=(), **environment):
        environment, marker = self.dns_environment(**environment)
        completed = subprocess.run(
            [self.executable],
            input=json.dumps(
                self.request(url="https://controller-utils-dns-test.invalid/file", mode=mode)
            )
            + "\n",
            capture_output=True,
            text=True,
            env=environment,
            pass_fds=pass_fds,
            timeout=10,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(marker.is_file(), "DNS delay hook did not run")
        result = json.loads(completed.stdout)
        self.assertEqual(
            result["fds_before"], result["fds_after"], result.get("descriptor_targets")
        )
        self.assertFalse(self.output.exists())
        return result, marker

    def test_worker_exit_is_reported_and_reaped(self):
        result, marker = self.delayed_dns(CT_DNS_TEST_EXIT="1")
        self.assertIn("worker stopped unexpectedly", result["error"])
        self.assertFalse(Path(f"/proc/{int(marker.read_text())}").exists())

    def test_worker_does_not_inherit_unrelated_descriptors(self):
        with self.output.with_name("unrelated").open("wb") as stream:
            descriptor = fcntl.fcntl(stream.fileno(), fcntl.F_DUPFD_CLOEXEC, 100)
        try:
            result, _ = self.delayed_dns(
                "cancel_dns",
                pass_fds=(descriptor,),
                CT_DNS_TEST_FORBIDDEN_FD=str(descriptor),
            )
            self.assertEqual(result["kind"], "cancelled")
        finally:
            os.close(descriptor)

    def test_worker_stops_if_application_dies(self):
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        environment, marker = self.dns_environment()
        process = subprocess.Popen(
            [self.executable],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
        worker = None
        try:
            process.stdin.write(
                json.dumps(self.request(url="https://controller-utils-dns-test.invalid/file"))
                + "\n"
            )
            process.stdin.flush()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if marker.exists() and marker.read_text():
                    break
                time.sleep(0.01)
            self.assertTrue(marker.exists(), "DNS delay hook did not run")
            worker = int(marker.read_text())
            process.kill()
            process.wait(timeout=2)
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                reaped, status = os.waitpid(worker, os.WNOHANG)
                if reaped:
                    worker = None
                    self.assertTrue(os.WIFSIGNALED(status))
                    self.assertEqual(os.WTERMSIG(status), 9)
                    break
                time.sleep(0.01)
            self.assertIsNone(worker, "Download worker survived its parent")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            if worker is not None:
                try:
                    os.kill(worker, 9)
                    os.waitpid(worker, 0)
                except ProcessLookupError:
                    pass
            process.stdin.close()
            process.stdout.close()
            process.stderr.close()
            self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)

    def test_total_response_header_limit(self):
        result = self.run_one("/many-headers")
        self.assertIn("header exceeded", result["error"])
        self.assertFalse(self.output.exists())

    def test_existing_files_and_symlinks_are_preserved(self):
        self.output.write_bytes(b"keep")
        self.assertIn("error", self.run_one())
        self.assertEqual(self.output.read_bytes(), b"keep")
        target = self.output.with_name("link")
        target.symlink_to(self.output)
        self.assertIn("error", self.run_one(destination=str(target)))
        self.assertTrue(target.is_symlink())
        self.assertEqual(self.output.read_bytes(), b"keep")

    def test_bad_urls_and_pre_cancelled_requests_create_nothing(self):
        start = len(self.server.requests)
        for url in ("http://127.0.0.1/", "file:///etc/passwd", self.server.base + "/?query=1"):
            self.assertIn("error", self.run_one(url=url))
        result = self.run_one(mode="already_cancelled")
        self.assertEqual(result["kind"], "cancelled")
        self.assertEqual(len(self.server.requests), start)
        self.assertFalse(self.output.exists())

    def test_cancel_preserves_a_replaced_output(self):
        process = subprocess.Popen(
            [self.executable],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=self.environment,
        )
        try:
            process.stdin.write(json.dumps(self.request("/hold", mode="replace_cancel")) + "\n")
            process.stdin.flush()
            self.assertTrue(self.server.held.wait(5))
            self.output.unlink()
            self.output.write_bytes(b"keep replacement")
            process.stdin.close()
            result = json.loads(process.stdout.readline())
            self.assertEqual(result["kind"], "cancelled")
            self.assertEqual(process.wait(timeout=5), 0)
            self.assertEqual(self.output.read_bytes(), b"keep replacement")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
            process.stderr.close()


if __name__ == "__main__":
    unittest.main()
