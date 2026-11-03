#!/usr/bin/env python3
# Connection helper for tests/e2e_concurrency.sh (audit convergence §6.4):
# ordinary HTTP/1.1 connections held open the way any keep-alive client does.
#
#   hold HOST PORT N SECS     open N connections, send one GET /ping on each,
#                             read the reply and keep them open (idle) for
#                             SECS, refreshing nothing. Prints "ok K" for the
#                             K connections that got 200, then holds.
#                             Writes "ready" to stdout once all are open.
#   probe HOST PORT           one connection, one GET /ping: prints the
#                             status code, or "refused" when the server
#                             closed it without a 200.
#   drip HOST PORT GAP SECS   send a request head one byte every GAP
#                             seconds; prints "closed T" (seconds after the
#                             connect) when the server closes it, or
#                             "open" if it is still open after SECS.
#   idle HOST PORT SECS       one request, then nothing: prints "closed T"
#                             when the server closes the connection, or
#                             "open" if it is still open after SECS.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import socket
import sys
import time


def connect(host, port, timeout=3.0):
    fam = socket.AF_INET6 if ":" in host else socket.AF_INET
    s = socket.socket(fam, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((host, port))
    return s


def get(s):
    s.sendall(b"GET /ping HTTP/1.1\r\nHost: t\r\n\r\n")
    data = b""
    while b"\r\n\r\n" not in data or not data.endswith(b"pong"):
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    line = data.split(b"\r\n", 1)[0].split(b" ")
    return line[1].decode() if len(line) > 1 else "refused"


def closed_by_peer(s, deadline):
    """Wait until the peer closes s (EOF or reset) or deadline passes."""
    while time.monotonic() < deadline:
        s.settimeout(max(0.05, min(0.25, deadline - time.monotonic())))
        try:
            if s.recv(4096) == b"":
                return True
        except socket.timeout:
            continue
        except OSError:
            return True
    return False


def main():
    cmd, host, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    if cmd == "hold":
        n, secs = int(sys.argv[4]), float(sys.argv[5])
        socks, ok = [], 0
        for _ in range(n):
            try:
                s = connect(host, port)
                if get(s) == "200":
                    ok += 1
                socks.append(s)
            except OSError:
                pass
        print("ok %d" % ok, flush=True)
        time.sleep(secs)
    elif cmd == "probe":
        try:
            s = connect(host, port)
            print(get(s))
        except OSError:
            print("refused")
    elif cmd == "drip":
        gap, secs = float(sys.argv[4]), float(sys.argv[5])
        t0 = time.monotonic()
        s = connect(host, port)
        head = b"GET /ping HTTP/1.1\r\nHost: t\r\nX-Pad: " + b"a" * 4096
        for b in head:
            try:
                s.sendall(bytes([b]))
            except OSError:
                print("closed %.1f" % (time.monotonic() - t0))
                return
            if closed_by_peer(s, time.monotonic() + gap):
                print("closed %.1f" % (time.monotonic() - t0))
                return
            if time.monotonic() - t0 > secs:
                print("open")
                return
        print("open")
    elif cmd == "idle":
        secs = float(sys.argv[4])
        s = connect(host, port)
        t0 = time.monotonic()
        get(s)
        if closed_by_peer(s, t0 + secs):
            print("closed %.1f" % (time.monotonic() - t0))
        else:
            print("open")


if __name__ == "__main__":
    main()
