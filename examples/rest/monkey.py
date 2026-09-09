#!/usr/bin/env python3
"""A monkey test for the rest example: random traffic, then chaos.

Phase 1 throws structure-aware nonsense at every route from several threads
at once -- wrong methods, junk paths, bodies that are almost the JSON the
handler wants, absent and oversized headers, raw bytes at the socket -- and
holds the server to one rule: it may refuse anything, but it may not fail.
Any 5xx, dropped connection, or unanswered request is a defect, because
every one of these inputs is the client's fault and the framework has a 4xx
for each.

Phase 2 (--compose) restarts postgres and redis underneath the running load.
5xx is expected while a dependency is gone, and for a moment after: a pooled
connection opened before the restart is a corpse, and the request that picks
it up is the one that finds out. So recovery is not "one flow worked" but
"the service converged" -- ten flows in a row with no failure -- and the
number of flows that failed on the way there is reported, because that
number is the cost of the drivers' lazy reconnect.

  python3 examples/rest/monkey.py http://localhost:8080
  python3 examples/rest/monkey.py http://localhost:8080 --compose examples/rest/docker-compose.yml

Standard library only; the seed makes a run reproducible.
"""

from __future__ import annotations

import argparse
import http.client
import json
import random
import socket
import string
import subprocess
import sys
import threading
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import quote, urlparse

# Methods the API defines, plus ones it does not: a route that only takes
# GET must say so rather than fall over.
METHODS = ["GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "TRACE", "FROBNICATE"]

# Path pieces that have broken servers before: traversal, encodings, nulls,
# very long segments, SQL and template fragments.
NASTY = [
    "..", "../..", "%2e%2e%2f", "%00", "%", "%zz", "//", "///", "?", "#",
    "'", '"', "`", ";", "|", "&&", "$(id)", "{{7*7}}", "<script>", "\\",
    "null", "undefined", "NaN", "-1", "0", "1e309", "9" * 40, "-" + "9" * 40,
    "éè", "\U0001f412", "a" * 2000, " ", "\t",
]

# Keys and values a JSON body might carry, right and wrong.
JSON_VALUES = [
    None, True, False, 0, -1, 1.5, 1e308, "", "x", "a" * 5000, [], {}, [1, 2, 3],
    {"nested": {"deep": [1, {"deeper": None}]}}, " ", "\U0001f412", "-1",
]
JSON_KEYS = ["username", "password", "title", "body", "id", "token", "", "__proto__", "a" * 200]


def _latin1(text: str) -> bool:
    try:
        text.encode("latin-1")
        return True
    except UnicodeEncodeError:
        return False


class Stats:
    """Status histogram and defect list, shared across threads."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.codes: Counter[str] = Counter()
        self.defects: list[str] = []
        self.requests = 0
        self.slowest = 0.0

    def record(self, code: object, elapsed: float, detail: str, tolerate_5xx: bool) -> None:
        with self.lock:
            self.requests += 1
            self.codes[str(code)] += 1
            self.slowest = max(self.slowest, elapsed)
            broken = code == "ERROR" or (isinstance(code, int) and code >= 500)
            if broken and not tolerate_5xx and len(self.defects) < 25:
                self.defects.append(f"{code}  {detail}")


class Monkey:
    def __init__(self, base: str, seed: int, stats: Stats) -> None:
        parsed = urlparse(base)
        self.host = parsed.hostname or "localhost"
        self.port = parsed.port or 80
        self.random = random.Random(seed)
        self.stats = stats
        self.tokens: list[str] = []

    # -- input generators -------------------------------------------------

    def word(self) -> str:
        return "".join(self.random.choices(string.ascii_lowercase + string.digits, k=self.random.randint(1, 12)))

    def path(self) -> str:
        pick = self.random.random()
        if pick < 0.30:
            return self.random.choice(["/posts", "/posts/", "/auth/register", "/auth/login"])
        if pick < 0.55:
            return "/posts/" + self.random.choice([str(self.random.randint(-5, 50)), self.random.choice(NASTY)])
        if pick < 0.70:
            return "/" + "/".join(self.random.choice(NASTY) for _ in range(self.random.randint(1, 4)))
        if pick < 0.85:
            base = self.random.choice(["/posts", "/auth/login", "/auth/register"])
            pairs = "&".join(f"{self.word()}={self.random.choice(NASTY)}" for _ in range(self.random.randint(1, 6)))
            return f"{base}?{pairs}"
        return "/" + self.word()

    def body(self) -> bytes:
        pick = self.random.random()
        if pick < 0.25:  # the shape a handler wants, sometimes valid
            return json.dumps({
                "username": self.word(),
                "password": self.word() * self.random.randint(1, 3),
                "title": self.word(),
                "body": self.word(),
            }).encode()
        if pick < 0.55:  # the right keys, the wrong types
            body = {self.random.choice(JSON_KEYS): self.random.choice(JSON_VALUES)
                    for _ in range(self.random.randint(1, 5))}
            return json.dumps(body).encode()
        if pick < 0.65:  # deeply nested, to test the parser's limits
            nested: object = "leaf"
            for _ in range(self.random.randint(10, 200)):
                nested = [nested] if self.random.random() < 0.5 else {"k": nested}
            return json.dumps(nested).encode()
        if pick < 0.80:  # malformed
            return self.random.choice([
                b"", b"{", b"}", b"[", b"null", b"nope", b'{"a":}', b'{"a" "b"}',
                b'{"a":1,}', b"\xff\xfe\x00\x01", b'{"a":' + b"9" * 400 + b"}",
            ])
        if pick < 0.90:  # big
            return json.dumps({"title": "x" * self.random.randint(10_000, 200_000)}).encode()
        return bytes(self.random.getrandbits(8) for _ in range(self.random.randint(0, 512)))

    def headers(self) -> dict[str, str]:
        headers: dict[str, str] = {}
        pick = self.random.random()
        if pick < 0.55:
            headers["content-type"] = "application/json"
        elif pick < 0.75:
            headers["content-type"] = self.random.choice(
                ["text/plain", "application/xml", "application/json; charset=weird", "", "///"])
        if self.random.random() < 0.45:
            if self.tokens and self.random.random() < 0.6:
                headers["authorization"] = "Bearer " + self.random.choice(self.tokens)
            else:
                headers["authorization"] = self.random.choice([
                    "Bearer " + self.word(), "Bearer", "Basic abc", "", "Bearer " + "f" * 5000,
                    "bearer lowercase", "Bearer é",
                ])
        if self.random.random() < 0.2:
            headers[self.word()] = self.random.choice(NASTY)
        # http.client encodes header values as latin-1; anything wider is a
        # raw-socket test, not a client one.
        return {k: v for k, v in headers.items() if _latin1(k) and _latin1(v)}

    # -- traffic ----------------------------------------------------------

    def request(self, tolerate_5xx: bool) -> None:
        method = self.random.choice(METHODS)
        # Percent-encoded, because http.client refuses to put a control
        # character or a non-ASCII byte on the request line at all. The
        # server still sees the byte, as %XX; the un-encoded spellings go
        # down the raw socket path below, where the bytes are ours to write.
        path = quote(self.path(), safe="/?&=%")
        body = self.body() if self.random.random() < 0.7 else None
        headers = self.headers()
        started = time.monotonic()
        try:
            conn = http.client.HTTPConnection(self.host, self.port, timeout=15)
            conn.request(method, path, body=body, headers=headers)
            response = conn.getresponse()
            payload = response.read(4096)
            code: object = response.status
            conn.close()
            # A token that comes back is worth reusing: it takes the fuzzer
            # down the authenticated paths rather than bouncing off 401.
            if response.status == 200 and b'"token"' in payload:
                try:
                    self.tokens.append(json.loads(payload)["token"])
                except Exception:
                    pass
        except Exception as exc:  # a refused or dropped connection is a defect
            code = "ERROR"
            payload = repr(exc).encode()[:200]
        self.stats.record(code, time.monotonic() - started,
                          f"{method} {path[:120]} body={len(body) if body else 0}B -> {payload[:120]!r}",
                          tolerate_5xx)

    def raw_socket_junk(self) -> None:
        """Bytes no HTTP client would send: not-HTTP at all, and request
        lines carrying raw control characters, NULs and UTF-8 that
        http.client will not encode. The listener must survive every one."""
        try:
            with socket.create_connection((self.host, self.port), timeout=5) as sock:
                sock.sendall(self.random.choice([
                    b"\x00\x01\x02\x03", b"GET\r\n\r\n", b"A" * 9000,
                    b"GET / HTTP/9.9\r\n\r\n", b"POST / HTTP/1.1\r\nContent-Length: 999999\r\n\r\nshort",
                    b"\r\n\r\n\r\n",
                    b"GET /posts/\x00 HTTP/1.1\r\nHost: x\r\n\r\n",
                    b"GET /posts/\t HTTP/1.1\r\nHost: x\r\n\r\n",
                    "GET /posts/\U0001f412 HTTP/1.1\r\nHost: x\r\n\r\n".encode(),
                    "GET /\u00e9\u00e8 HTTP/1.1\r\nHost: \U0001f412\r\n\r\n".encode(),
                    b"FROBNICATE / HTTP/1.1\r\nHost: x\r\n\r\n",
                    b"GET / HTTP/1.1\r\n" + b"X-Pad: " + b"z" * 20000 + b"\r\n\r\n",
                ]))
                sock.recv(512)
        except Exception:
            pass  # the server closing on garbage is correct; only /posts must still work


def valid_flow(base: str) -> tuple[bool, str]:
    """Register, log in, post, read back. The proof the server really works."""
    parsed = urlparse(base)
    host, port = parsed.hostname or "localhost", parsed.port or 80
    user = "monkey" + "".join(random.choices(string.ascii_lowercase, k=10))
    json_header = {"content-type": "application/json"}

    def call(method: str, path: str, body: object = None, headers: dict[str, str] | None = None):
        conn = http.client.HTTPConnection(host, port, timeout=20)
        conn.request(method, path, body=json.dumps(body) if body is not None else None,
                     headers=headers or {})
        response = conn.getresponse()
        payload = response.read()
        conn.close()
        return response.status, payload

    try:
        status, _ = call("POST", "/auth/register", {"username": user, "password": "hunter2hunter2"}, json_header)
        if status != 201:
            return False, f"register -> {status}"
        status, payload = call("POST", "/auth/login", {"username": user, "password": "hunter2hunter2"}, json_header)
        if status != 200:
            return False, f"login -> {status}"
        token = json.loads(payload)["token"]
        auth = {"content-type": "application/json", "authorization": "Bearer " + token}
        status, payload = call("POST", "/posts", {"title": "monkey", "body": "survived"}, auth)
        if status != 201:
            return False, f"create post -> {status}"
        post_id = json.loads(payload)["id"]
        status, _ = call("GET", f"/posts/{post_id}")
        if status != 200:
            return False, f"read post -> {status}"
        return True, f"user {user}, post {post_id}"
    except Exception as exc:
        return False, repr(exc)


def converge(base: str, streak_wanted: int = 10, attempts: int = 60) -> tuple[bool, int, str]:
    """Run flows until enough succeed in a row. Answers whether it got
    there, how many failed on the way, and the last detail."""
    failures, streak, detail = 0, 0, ""
    for _ in range(attempts):
        ok, detail = valid_flow(base)
        if ok:
            streak += 1
            if streak >= streak_wanted:
                return True, failures, detail
        else:
            failures += 1
            streak = 0
            time.sleep(0.5)
    return False, failures, detail


def storm(base: str, requests: int, workers: int, seed: int, tolerate_5xx: bool) -> Stats:
    stats = Stats()

    def worker(index: int) -> None:
        monkey = Monkey(base, seed + index, stats)
        for i in range(requests // workers):
            if i % 40 == 39:
                monkey.raw_socket_junk()
            else:
                monkey.request(tolerate_5xx)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        list(pool.map(worker, range(workers)))
    return stats


def report(name: str, stats: Stats, elapsed: float) -> None:
    codes = ", ".join(f"{code}: {count}" for code, count in sorted(stats.codes.items()))
    print(f"  {stats.requests} requests in {elapsed:.1f}s  ({stats.requests / max(elapsed, 0.001):.0f}/s), "
          f"slowest {stats.slowest * 1000:.0f}ms")
    print(f"  statuses: {codes}")
    if stats.defects:
        print(f"  DEFECTS in {name}:")
        for defect in stats.defects:
            print(f"    {defect}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("base", nargs="?", default="http://localhost:8080")
    parser.add_argument("--requests", type=int, default=2000)
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--compose", help="docker compose file; enables the chaos phase")
    args = parser.parse_args()

    ok, detail = valid_flow(args.base)
    if not ok:
        print(f"the server is not healthy before the run: {detail}")
        return 1
    print(f"baseline flow ok ({detail})\n")

    print(f"phase 1: {args.requests} random requests over {args.workers} threads, seed {args.seed}")
    started = time.monotonic()
    stats = storm(args.base, args.requests, args.workers, args.seed, tolerate_5xx=False)
    report("phase 1", stats, time.monotonic() - started)
    failed = bool(stats.defects)

    ok, detail = valid_flow(args.base)
    print(f"  flow after the storm: {'ok' if ok else 'BROKEN -- ' + detail}\n")
    failed = failed or not ok

    if args.compose:
        compose = ["docker", "compose", "-f", args.compose]
        for service in ("postgres", "redis"):
            print(f"phase 2: restarting {service} under load")
            started = time.monotonic()
            thread = threading.Thread(
                target=lambda: storm(args.base, 400, 8, args.seed + 100, tolerate_5xx=True))
            thread.start()
            subprocess.run(compose + ["restart", service], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            thread.join()
            ok, failures, detail = converge(args.base)
            state = "converged" if ok else "NEVER CONVERGED -- " + detail
            print(f"  {state} in {time.monotonic() - started:.1f}s; "
                  f"{failures} flow(s) failed first, one per connection that predated the restart\n")
            failed = failed or not ok

    print("MONKEY TEST FAILED" if failed else "MONKEY TEST PASSED")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
