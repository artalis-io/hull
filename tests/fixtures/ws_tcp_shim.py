#!/usr/bin/env python3
"""A WebSocket-to-TCP relay: the server half of what a tunnel provider does.

`cloudflared access ssh` is a WebSocket carrying a raw TCP stream, with the
authentication in two request headers. This is that, in the small: it accepts
an RFC 6455 upgrade, opens a TCP connection to --target, and pumps bytes both
ways until either side closes.

It exists so tests/e2e_ssh_tunnel.sh can put a REAL socket and REAL frames
under hull/ssh without needing a Cloudflare account. Nothing here is
Cloudflare-specific - the provider's headers are just headers, which is the
point being demonstrated.

Deliberately dependency-free (no `websockets` package): CI installs nothing
for it, and the framing is short enough to read.

    ws_tcp_shim.py --listen 9101 --target-host 127.0.0.1 --target-port 2222
                   --require-header "Cf-Access-Client-Id: abc.access"
                   --headers-out /tmp/seen.txt --ready-file /tmp/ready

--require-header makes the relay answer 403 unless the header is present and
exact, which is the shape of an Access policy rejection and the one failure a
caller has to tell apart from its own manifest refusing.

SPDX-License-Identifier: AGPL-3.0-or-later
"""

import argparse
import base64
import hashlib
import select
import socket
import struct
import sys
import threading

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_CONT = 0x0
OP_TEXT = 0x1
OP_BIN = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA

# A client frame we will hold before deciding the peer is unreasonable. The
# length field is 64 bits and entirely peer-controlled, so it is checked
# before anything of that size is allocated.
MAX_FRAME = 4 * 1024 * 1024


def recv_exactly(sock, n):
    """Read exactly n bytes, or return None if the peer closed first."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_request(sock):
    """Read an HTTP request head, plus whatever arrived glued behind it.

    Bounded: a peer that never sends the blank line must not make us buffer
    without limit."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        if len(buf) > 64 * 1024:
            return None
        chunk = sock.recv(4096)
        if not chunk:
            return None
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    return head.decode("latin-1"), rest


def parse_headers(head):
    lines = head.split("\r\n")
    request_line = lines[0] if lines else ""
    headers = {}
    ordered = []
    for line in lines[1:]:
        if ":" not in line:
            continue
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
        ordered.append(name.strip() + ": " + value.strip())
    return request_line, headers, ordered


def send_frame(sock, opcode, payload):
    """One server frame. NOT masked: RFC 6455 section 5.1 forbids a server
    masking, and hull.web.ws-stream refuses a masked server frame - correctly,
    so getting this wrong here would look like a client bug."""
    n = len(payload)
    if n < 126:
        header = struct.pack("!BB", 0x80 | opcode, n)
    elif n < (1 << 16):
        header = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        header = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    sock.sendall(header + payload)


def read_frame(sock):
    """One client frame -> (opcode, payload), or None at EOF.

    Raises on anything a conformant client would not send: tolerating it here
    would let a client bug pass the test silently."""
    head = recv_exactly(sock, 2)
    if head is None:
        return None
    b0, b1 = head[0], head[1]
    fin = b0 & 0x80
    opcode = b0 & 0x0F
    masked = b1 & 0x80
    length = b1 & 0x7F

    if not masked:
        raise ValueError("client frame is not masked (RFC 6455 section 5.1)")

    if length == 126:
        ext = recv_exactly(sock, 2)
        if ext is None:
            return None
        length = struct.unpack("!H", ext)[0]
    elif length == 127:
        ext = recv_exactly(sock, 8)
        if ext is None:
            return None
        length = struct.unpack("!Q", ext)[0]

    if length > MAX_FRAME:
        raise ValueError("client frame of %d bytes exceeds the cap" % length)
    if opcode in (OP_CLOSE, OP_PING, OP_PONG) and (length > 125 or not fin):
        raise ValueError("control frame must be <=125 bytes and unfragmented")

    mask = recv_exactly(sock, 4)
    if mask is None:
        return None
    payload = recv_exactly(sock, length) if length else b""
    if payload is None:
        return None
    unmasked = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))
    return opcode, unmasked


class FramePrefix:
    """A socket that serves already-buffered bytes before reading the real one.

    hull.web.ws-stream can frame its first write into the same segment as the
    upgrade request, so those bytes arrive with the head and must be read back
    before the socket is touched again. Dropping them would lose the client's
    first SSH bytes on a fast peer, and only sometimes."""

    def __init__(self, sock, prefix):
        self._sock = sock
        self._buf = prefix

    def recv(self, n):
        if self._buf:
            out, self._buf = self._buf[:n], self._buf[n:]
            return out
        return self._sock.recv(n)

    def pending(self):
        return len(self._buf) > 0

    def sendall(self, data):
        return self._sock.sendall(data)

    def fileno(self):
        return self._sock.fileno()

    def close(self):
        return self._sock.close()


def pump(ws, tcp):
    """Relay until either side finishes. WebSocket payloads go out as raw TCP
    bytes; TCP reads come back as one binary frame each."""
    try:
        while True:
            # Buffered bytes are not visible to select(), so drain them first
            # or the relay would block on a socket whose data it already has.
            if ws.pending():
                readable = [ws]
            else:
                readable, _, _ = select.select([ws, tcp], [], [], 30)
                if not readable:
                    return                  # idle: the test has moved on
            if ws in readable:
                frame = read_frame(ws)
                if frame is None:
                    return
                opcode, payload = frame
                if opcode in (OP_BIN, OP_TEXT, OP_CONT):
                    if payload:
                        tcp.sendall(payload)
                elif opcode == OP_PING:
                    send_frame(ws, OP_PONG, payload)
                elif opcode == OP_CLOSE:
                    try:
                        send_frame(ws, OP_CLOSE, b"")
                    except OSError:
                        pass
                    return
            if tcp in readable:
                data = tcp.recv(65536)
                if not data:
                    try:
                        send_frame(ws, OP_CLOSE, b"")
                    except OSError:
                        pass
                    return
                send_frame(ws, OP_BIN, data)
    except (OSError, ValueError) as exc:
        print("shim: relay ended: %s" % exc, file=sys.stderr)


def handle(conn, args):
    try:
        got = read_request(conn)
        if got is None:
            return
        head, leftover = got
        request_line, headers, ordered = parse_headers(head)

        if args.headers_out:
            # Appended, not overwritten: the test connects more than once
            # (accept the host key, then reconnect) and wants to see both.
            with open(args.headers_out, "a", encoding="utf-8") as fh:
                fh.write(request_line + "\n")
                for line in ordered:
                    fh.write(line + "\n")
                fh.write("--\n")

        for required in args.require_header or []:
            name, _, value = required.partition(":")
            if headers.get(name.strip().lower()) != value.strip():
                body = b"forbidden\n"
                conn.sendall(
                    b"HTTP/1.1 403 Forbidden\r\nContent-Length: "
                    + str(len(body)).encode()
                    + b"\r\nConnection: close\r\n\r\n" + body)
                return

        key = headers.get("sec-websocket-key")
        if (headers.get("upgrade", "").lower() != "websocket"
                or "upgrade" not in headers.get("connection", "").lower()
                or not key):
            conn.sendall(b"HTTP/1.1 400 Bad Request\r\n"
                         b"Content-Length: 0\r\nConnection: close\r\n\r\n")
            return

        accept = base64.b64encode(
            hashlib.sha1(key.encode("latin-1") + GUID).digest()).decode()
        conn.sendall(
            b"HTTP/1.1 101 Switching Protocols\r\n"
            b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            b"Sec-WebSocket-Accept: " + accept.encode() + b"\r\n\r\n")

        tcp = socket.create_connection((args.target_host, args.target_port), 10)
        try:
            pump(FramePrefix(conn, leftover), tcp)
        finally:
            try:
                tcp.close()
            except OSError:
                pass
    except (OSError, ValueError) as exc:
        print("shim: connection ended: %s" % exc, file=sys.stderr)
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, required=True)
    ap.add_argument("--target-host", default="127.0.0.1")
    ap.add_argument("--target-port", type=int, required=True)
    ap.add_argument("--require-header", action="append")
    ap.add_argument("--headers-out")
    ap.add_argument("--ready-file")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.listen))
    srv.listen(8)

    if args.ready_file:
        # Written AFTER listen(), so the test waits on the socket actually
        # existing rather than on a sleep.
        with open(args.ready_file, "w", encoding="utf-8") as fh:
            fh.write("%d\n" % args.listen)

    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        # A thread per connection: FramePrefix has per-connection state, and
        # the test opens a second connection after accepting the host key.
        threading.Thread(target=handle, args=(conn, args), daemon=True).start()


if __name__ == "__main__":
    main()
