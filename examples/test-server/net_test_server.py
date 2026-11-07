"""Test server for the hardware test layouts in examples/ (net-test.json, activity-test.mjs, script-test.mjs).

Run on the PC:  python examples/test-server/net_test_server.py
HTTP on :8000, UDP listeners on :9 (WoL) and :9999. Allow it through the Windows firewall.
Each line shows the client's source port, so a repeated port = a reused connection.
"""
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def ts():
    # Milliseconds, for the latency checks (script-test.mjs "Latency op+script")
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}"


mute_state = "0"                            # /mute: GET reads it, POST "mute=0|1" sets it


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"          # keep-alive

    def log_message(self, fmt, *args):
        pass

    def reply(self, code, body=b"ok", extra=None):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def show(self, what):
        print(f"{ts()} [{self.client_address[1]}] {what} {self.path}")
        for k, v in self.headers.items():
            if k.lower() not in ("host", "accept"):
                print(f"            {k}: {v}")

    def do_GET(self):
        self.show("GET")
        if self.path.startswith("/redirect"):
            self.reply(302, b"", {"Location": "/get?redirected=1"})
        elif self.path.startswith("/hang"):
            time.sleep(60)                  # accepts, never answers
        elif self.path.startswith("/slow"):
            time.sleep(3)
            self.reply(200)
        elif self.path.startswith("/big"):
            size = 5 * 1024 * 1024
            self.send_response(200)
            self.send_header("Content-Length", str(size))
            self.end_headers()
            chunk = b"x" * 65536
            try:
                for _ in range(size // len(chunk)):
                    self.wfile.write(chunk)
                print(f"{ts()}            /big fully sent")
            except OSError:
                print(f"{ts()}            /big client went away")
        elif self.path.startswith("/get"):
            self.reply(200)
        elif self.path.startswith("/mute"):
            self.reply(200, f"mute={mute_state}".encode())
        else:
            self.reply(404, b"not found")

    def do_POST(self):
        global mute_state
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        self.show("POST")
        print(f"            body ({n} bytes): {body!r}")
        if self.path.startswith("/mute") and body.startswith(b"mute="):
            mute_state = body[5:6].decode()
        self.reply(200)


def udp_listener(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("", port))
    while True:
        data, addr = s.recvfrom(2048)
        extra = ""
        if len(data) == 102 and data[:6] == b"\xff" * 6:
            extra = f"  WoL magic, mac={data[6:12].hex(':')}, repeats ok={data[6:] == data[6:12] * 16}"
        print(f"{ts()} UDP :{port} from {addr[0]}:{addr[1]} {len(data)} bytes {data[:32]!r}{extra}")


for p in (9, 9999):
    threading.Thread(target=udp_listener, args=(p,), daemon=True).start()
print("HTTP :8000, UDP :9 and :9999 - Ctrl+C to stop")
ThreadingHTTPServer(("", 8000), Handler).serve_forever()
