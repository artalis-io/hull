#!/usr/bin/env python3
"""WebSocket close cycles for tests/e2e_ws_close.sh (audit 44 M5).

    ws_cycle.py PORT PATH SEND_MSG COUNT

Upgrades on PATH, optionally sends one text frame (SEND_MSG = 1), waits for
the server's Close, answers it with the same code, and reads to EOF; COUNT
times. Prints "CYCLES <n>" with the number of cycles the server closed.
Dependency-free; frames are masked as RFC 6455 requires of a client.

SPDX-License-Identifier: AGPL-3.0-or-later
"""

import base64
import os
import socket
import sys


def recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b


def masked(opcode, payload):
    mask = os.urandom(4)
    body = bytes(p ^ mask[i % 4] for i, p in enumerate(payload))
    return bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + body


def cycle(port, path, send_msg):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    try:
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall(("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                   "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: " + key + "\r\n"
                   "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            c = s.recv(1)
            if not c:
                return False
            head += c
        if b" 101 " not in head:
            return False
        if send_msg:
            s.sendall(masked(0x1, b"bye"))
        code = None
        try:
            while True:
                h = recv_exact(s, 2)
                op, n = h[0] & 0x0F, h[1] & 0x7F
                if n == 126:
                    n = int.from_bytes(recv_exact(s, 2), "big")
                payload = recv_exact(s, n) if n else b""
                if op == 0x8:
                    code = payload[:2]
                    break
        except (EOFError, socket.timeout):
            return False
        s.sendall(masked(0x8, code))
        try:
            while s.recv(4096):
                pass
        except OSError:
            pass
        return True
    finally:
        s.close()


def main():
    port, path = int(sys.argv[1]), sys.argv[2]
    send_msg, count = sys.argv[3] == "1", int(sys.argv[4])
    ok = sum(1 for _ in range(count) if cycle(port, path, send_msg))
    print("CYCLES %d" % ok, flush=True)


if __name__ == "__main__":
    main()
