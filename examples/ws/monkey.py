#!/usr/bin/env python3
"""Monkey test for examples/ws: many users at once, plus junk.

Holds the server to one rule: it may refuse a bad handshake, but a live
socket must not 5xx, drop mid-session, or leave the process dead.

  python3 examples/ws/monkey.py http://127.0.0.1:8080

Standard library only.
"""

from __future__ import annotations

import argparse
import base64
import os
import random
import socket
import struct
import sys
import threading
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from urllib.parse import urlparse

OP_TEXT = 0x1
OP_CLOSE = 0x8


class Stats:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.codes: Counter[str] = Counter()
        self.defects: list[str] = []
        self.ok = 0
        self.requests = 0

    def record(self, code: str, detail: str, defect: bool) -> None:
        with self.lock:
            self.requests += 1
            self.codes[code] += 1
            if defect:
                if len(self.defects) < 25:
                    self.defects.append(f"{code}  {detail}")
            else:
                self.ok += 1


class Conn:
    def __init__(self, host: str, port: int, timeout: float) -> None:
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = bytearray()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def send_all(self, data: bytes) -> None:
        self.sock.sendall(data)

    def _fill(self, n: int) -> bool:
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                return False
            self.buf.extend(chunk)
        return True

    def read_http(self) -> tuple[int, bytes]:
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                return 0, b""
            self.buf.extend(chunk)
        head, _, rest = bytes(self.buf).partition(b"\r\n\r\n")
        self.buf[:] = rest
        line = head.split(b"\r\n", 1)[0].decode("latin-1", "replace")
        parts = line.split(" ")
        status = int(parts[1]) if len(parts) > 1 else 0
        return status, head

    def handshake(self, path: str) -> int:
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {self.sock.getpeername()[0]}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        self.send_all(req.encode("ascii"))
        status, _ = self.read_http()
        return status

    def send_frame(self, opcode: int, payload: bytes) -> None:
        n = len(payload)
        hdr = bytes([0x80 | opcode])
        if n < 126:
            hdr += bytes([0x80 | n])
        elif n < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack("!H", n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack("!Q", n)
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.send_all(hdr + mask + masked)

    def read_frame(self) -> tuple[int, bytes] | None:
        if not self._fill(2):
            return None
        b0, b1 = self.buf[0], self.buf[1]
        opcode = b0 & 0x0F
        n = b1 & 0x7F
        off = 2
        if n == 126:
            if not self._fill(4):
                return None
            n = struct.unpack("!H", self.buf[2:4])[0]
            off = 4
        elif n == 127:
            if not self._fill(10):
                return None
            n = struct.unpack("!Q", self.buf[2:10])[0]
            off = 10
        if not self._fill(off + n):
            return None
        payload = bytes(self.buf[off : off + n])
        del self.buf[: off + n]
        return opcode, payload


def user_session(host: str, port: int, path: str, sends: int, rng: random.Random, stats: Stats, timeout: float) -> None:
    detail = f"WS {path}"
    conn = None
    try:
        conn = Conn(host, port, timeout)
        status = conn.handshake(path)
        if status != 101:
            stats.record(str(status), detail, defect=status >= 500 or status == 0)
            return
        for i in range(sends):
            conn.send_frame(OP_TEXT, f"u{rng.randrange(1_000_000)}:{i}".encode())
            got = conn.read_frame()
            if got is None:
                stats.record("DROP", detail, defect=True)
                return
        conn.send_frame(OP_CLOSE, b"")
        conn.read_frame()
        stats.record("101", detail, defect=False)
    except (TimeoutError, socket.timeout, OSError) as exc:
        stats.record("ERROR", f"{detail} {exc}", defect=True)
    finally:
        if conn is not None:
            conn.close()


def junk(host: str, port: int, stats: Stats, rng: random.Random, timeout: float) -> None:
    kind = rng.choice(["get-chat", "get-echo", "bad-key", "no-upgrade"])
    conn = None
    try:
        conn = Conn(host, port, timeout)
        if kind == "get-chat":
            conn.send_all(b"GET /chat HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            status, _ = conn.read_http()
            stats.record(str(status), "GET /chat", defect=status >= 500 or status == 0)
        elif kind == "get-echo":
            conn.send_all(b"GET /echo/a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            status, _ = conn.read_http()
            stats.record(str(status), "GET /echo", defect=status not in (404, 405) and status >= 500 or status == 0)
        elif kind == "bad-key":
            conn.send_all(
                b"GET /chat HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                b"Connection: Upgrade\r\nSec-WebSocket-Key: short\r\n"
                b"Sec-WebSocket-Version: 13\r\n\r\n"
            )
            status, _ = conn.read_http()
            stats.record(str(status), "bad-key", defect=status not in (400, 0) and status >= 500)
        else:
            conn.send_all(b"GET /chat HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            status, _ = conn.read_http()
            stats.record(str(status), "no-upgrade", defect=status >= 500 or status == 0)
    except (TimeoutError, socket.timeout, OSError) as exc:
        stats.record("ERROR", f"junk {kind} {exc}", defect=True)
    finally:
        if conn is not None:
            conn.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("base", nargs="?", default="http://127.0.0.1:8080")
    parser.add_argument("--users", type=int, default=16)
    parser.add_argument("--parallel", type=int, default=0, help="in-flight sockets; 0 means --users")
    parser.add_argument("--sends", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    parsed = urlparse(args.base)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 80
    parallel = args.parallel if args.parallel > 0 else args.users
    rng = random.Random(args.seed)
    stats = Stats()
    jobs = []
    with ThreadPoolExecutor(max_workers=parallel) as pool:
        for i in range(args.users):
            path = rng.choice([f"/chat/r{i % 5}", f"/echo/u{i}", f"/rooms/{i % 5}"])
            jobs.append(pool.submit(user_session, host, port, path, args.sends, random.Random(args.seed + i), stats, args.timeout))
        for _ in range(min(64, parallel)):
            jobs.append(pool.submit(junk, host, port, stats, random.Random(rng.randrange(1 << 30)), args.timeout))
        for fut in as_completed(jobs):
            fut.result()
    print(f"{stats.requests} exchanges  ok={stats.ok}  {dict(stats.codes)}")
    if stats.defects:
        print("defects:")
        for row in stats.defects:
            print(f"  {row}")
        return 1
    print("no defects")
    return 0


if __name__ == "__main__":
    sys.exit(main())
