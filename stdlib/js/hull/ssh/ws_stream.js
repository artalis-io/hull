// hull:ssh:ws_stream - a WebSocket client that behaves like a byte stream.
//
// The JS twin of hull.ssh.ws_stream (stdlib/lua/hull/ssh/ws_stream.lua); see
// there for the reasoning. RFC 6455, client side only. Turns a stream
// (read/write/close, the duck-typed shape hull:ssh:_io hands out) carrying a
// WebSocket into a stream carrying the bytes inside it, which is the interface
// hull:ssh:transport already takes.
//
// It holds no capability. The stream, and the random and sha1 functions, are
// all passed in - so this module cannot open a connection, only transform one
// it was handed.
//
// Multiple returns are arrays (see the per-function comments); the stream
// methods follow the stream interface: read -> [chunk] | [null, msg, code],
// write -> [true] | [null, msg, code].

import { encoding } from "hull:encoding";
import { wire } from "hull:ssh:wire";
import { _refusal } from "hull:_refusal";

const base64 = encoding.base64;

// RFC 6455 section 1.3.
const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const OP_CONT  = 0x0;
const OP_TEXT  = 0x1;
const OP_BIN   = 0x2;
const OP_CLOSE = 0x8;
const OP_PING  = 0x9;
const OP_PONG  = 0xA;

// A frame we will hold in memory before deciding the peer is unreasonable;
// checked before anything of that size is touched.
const MAX_FRAME = 4 * 1024 * 1024;

// Control frames carry at most 125 bytes and are never fragmented.
const MAX_CONTROL = 125;

const errText = (e) => (e && typeof e === "object" && typeof e.message === "string")
    ? e.message : String(e);

function u16be(v) {
    return String.fromCharCode((v >>> 8) & 255, v & 255);
}

function u64be(v) {
    const hi = Math.floor(v / 0x100000000);
    const lo = v - hi * 0x100000000;
    return String.fromCharCode((hi >>> 24) & 255, (hi >>> 16) & 255, (hi >>> 8) & 255, hi & 255,
        (lo >>> 24) & 255, (lo >>> 16) & 255, (lo >>> 8) & 255, lo & 255);
}

// The 64-bit length at `p`; throws where Lua's signed unpack went negative.
function readLen64(buf, p) {
    const hi = wire.peekUint32(buf, p);
    const lo = wire.peekUint32(buf, p + 4);
    if (hi >= 0x80000000) {
        throw new Error("ssh.ws_stream: frame length exceeds the representable range");
    }
    return hi * 0x100000000 + lo;
}

// Handshake ------------------------------------------------------------------

// The client nonce: 16 random bytes, base64 WITH padding (section 4.1).
function key(randomBytes) {
    const raw = randomBytes(16);
    if (typeof raw !== "string" || raw.length !== 16) {
        throw new TypeError("ssh.ws_stream: random_bytes must return 16 bytes");
    }
    return base64.encode(raw);
}

// What the server must answer with (section 4.2.2 step 5).
function accept(sha1Raw, k) {
    return base64.encode(sha1Raw(k + GUID));
}

// Build the upgrade request. `headers` is an array of "Name: value" strings.
function buildRequest(opts) {
    if (typeof opts.host !== "string" || opts.host === "") {
        throw new TypeError("ssh.ws_stream: a host is required");
    }
    if (typeof opts.key !== "string" || opts.key === "") {
        throw new TypeError("ssh.ws_stream: a key is required");
    }
    // CR, LF or NUL anywhere in a value injects a line of its own (or reads as
    // a terminator), so every interpolated part is checked.
    // A value that is not a primitive string is refused outright: it would be
    // turned into text only when the lines are joined, past this check.
    const noCrlf = (what, v) => {
        if (typeof v !== "string") {
            throw new TypeError("ssh.ws_stream: " + what + " must be a string");
        }
        if (/[\r\n\x00]/.test(v)) {
            throw new TypeError("ssh.ws_stream: " + what + " contains CR, LF or NUL: "
                + v.replace(/[\r\n\x00]/g, "?"));
        }
        return v;
    };

    // Origin-form only (RFC 9112 section 3.2.2; audit 22).
    const path = (opts.path === undefined || opts.path === null) ? "/" : opts.path;
    if (typeof path !== "string" || path.slice(0, 1) !== "/" || path.slice(0, 2) === "//"
        || /[\s\x00-\x1f\x7f]/.test(path)) {
        throw new TypeError("ssh.ws_stream: path must start with one '/' and hold no spaces"
            + " or control characters");
    }

    const lines = [
        "GET " + noCrlf("path", path) + " HTTP/1.1",
        "Host: " + noCrlf("host", opts.host),
        "Upgrade: websocket",
        "Connection: Upgrade",
        "Sec-WebSocket-Key: " + noCrlf("key", opts.key),
        "Sec-WebSocket-Version: 13",
    ];
    for (const h of (opts.headers || [])) lines.push(noCrlf("header", h));
    return lines.join("\r\n") + "\r\n\r\n";
}

// Parse the response head. -> [{ status, headers }] with header names
// lowercased, or [null, reason].
function parseResponse(text) {
    const end = text.indexOf("\r\n\r\n");
    if (end < 0) return [null, "incomplete"];
    const head = text.slice(0, end);
    const m = /^([^\r\n]*)\r\n?([\s\S]*)$/.exec(head);
    if (!m) return [null, "malformed status line"];
    const first = m[1], rest = m[2];
    const sm = /^HTTP\/1\.[01] (\d\d\d)/.exec(first);
    if (!sm) return [null, "malformed status line"];
    const status = Number(sm[1]);

    const headers = Object.create(null);
    for (const line of (rest || "").split(/[\r\n]+/)) {
        if (line === "") continue;
        const hm = /^([^:]+):\s*(.*)$/.exec(line);
        if (hm) headers[hm[1].toLowerCase()] = hm[2];
    }
    return [{ status, headers }];
}

// Framing --------------------------------------------------------------------

// XOR a payload with the 4-byte mask, in chunks.
function applyMask(data, mask) {
    if (data.length === 0) return data;
    const m = [mask.charCodeAt(0), mask.charCodeAt(1), mask.charCodeAt(2), mask.charCodeAt(3)];
    const out = [];
    const n = data.length;
    for (let i = 0; i < n; i += 1024) {
        const j = Math.min(i + 1024, n);
        const b = new Array(j - i);
        for (let k = i; k < j; k++) b[k - i] = data.charCodeAt(k) ^ m[k % 4];
        out.push(String.fromCharCode.apply(null, b));
    }
    return out.join("");
}

// Encode one CLIENT frame. A client MUST mask every frame (section 5.3).
function encode(opcode, payload, mask) {
    if (payload === undefined || payload === null) payload = "";
    if (typeof mask !== "string" || mask.length !== 4) {
        throw new TypeError("ssh.ws_stream: a 4-byte mask is required");
    }
    if (opcode >= 0x8 && payload.length > MAX_CONTROL) {
        throw new RangeError("ssh.ws_stream: a control frame carries at most 125 bytes");
    }

    const b1 = 0x80 | (opcode & 0x0F);          // FIN set; no fragmentation
    const n = payload.length;
    let head;
    if (n < 126) head = String.fromCharCode(b1, 0x80 | n);
    else if (n < 0x10000) head = String.fromCharCode(b1, 0x80 | 126) + u16be(n);
    else head = String.fromCharCode(b1, 0x80 | 127) + u64be(n);
    return head + mask + applyMask(payload, mask);
}

// How many bytes the frame at the front of `buf` occupies in total.
// -> [total] | [null, "need_more"]
function frameSize(buf) {
    if (buf.length < 2) return [null, "need_more"];
    let len = buf.charCodeAt(1) & 0x7F;
    let head = 2;
    if (len === 126) {
        if (buf.length < 4) return [null, "need_more"];
        len = (buf.charCodeAt(2) << 8) | buf.charCodeAt(3); head = 4;
    } else if (len === 127) {
        if (buf.length < 10) return [null, "need_more"];
        len = readLen64(buf, 2); head = 10;
    }
    if (len > MAX_FRAME) {
        throw new Error("ssh.ws_stream: frame of " + String(len) + " bytes exceeds the maximum");
    }
    return [head + len];
}

// Decode one frame from the front of `buf`.
// -> [frame, consumed] | [null, "need_more"]; throws on anything a
// conformant server would not send.
function decode(buf) {
    if (buf.length < 2) return [null, "need_more"];

    const b1 = buf.charCodeAt(0), b2 = buf.charCodeAt(1);
    const fin    = (b1 & 0x80) !== 0;
    const rsv    = b1 & 0x70;
    const opcode = b1 & 0x0F;
    const masked = (b2 & 0x80) !== 0;
    let len      = b2 & 0x7F;
    let pos      = 2;                            // 0-based (Lua: 3)

    if (rsv !== 0) throw new Error("ssh.ws_stream: reserved bits set without an extension");
    if (masked) throw new Error("ssh.ws_stream: server sent a masked frame");

    if (len === 126) {
        if (buf.length < pos + 2) return [null, "need_more"];
        len = (buf.charCodeAt(pos) << 8) | buf.charCodeAt(pos + 1); pos += 2;
    } else if (len === 127) {
        if (buf.length < pos + 8) return [null, "need_more"];
        len = readLen64(buf, pos); pos += 8;
    }

    // Bounded BEFORE waiting for the bytes.
    if (len > MAX_FRAME) {
        throw new Error("ssh.ws_stream: frame of " + String(len) + " bytes exceeds the maximum");
    }
    if (opcode >= 0x8) {
        if (len > MAX_CONTROL) {
            throw new Error("ssh.ws_stream: control frame of " + String(len) + " bytes exceeds 125");
        }
        if (!fin) throw new Error("ssh.ws_stream: control frame must not be fragmented");
    }

    if (buf.length < pos + len) return [null, "need_more"];
    const payload = buf.slice(pos, pos + len);
    return [{ fin, opcode, payload }, pos + len];
}

// The stream adapter ---------------------------------------------------------

class Stream {
    constructor(fields) {
        Object.assign(this, fields);
    }

    // Ensure the buffer holds at least n bytes. -> [true] | [false] at EOF
    // (not a throw: for a byte stream a transport that ends is EOF) |
    // [null, err, code].
    async _need(n) {
        const [buf, ok, err, code] = await wire.gather(this.inbuf, n, (m) => this.s.read(m));
        this.inbuf = buf;
        if (ok === false) this.closed = true;
        if (ok) return [true];
        return [ok, err, code];
    }

    // Pull frames until at least one byte of application data is buffered, or
    // the peer closes. Control frames are handled here and never surface.
    // -> [true] | [null, err, code]
    async _pump() {
        while (this.opos >= this.out.length && !this.closed) {
            let [ok, err, code] = await this._need(2);
            if (ok === null || ok === undefined) return [null, err, code];
            if (!ok) break;                          // EOF

            // Grow through an extended-length header until it can be read.
            let [total] = frameSize(this.inbuf);
            while (total === null) {
                let more;
                [more, err, code] = await this._need(this.inbuf.length + 1);
                if (more === null || more === undefined) return [null, err, code];
                if (!more) break;                    // EOF inside the header
                [total] = frameSize(this.inbuf);
            }
            if (total === null) break;

            let got;
            [got, err, code] = await this._need(total);
            if (got === null || got === undefined) return [null, err, code];
            if (!got) break;                         // EOF mid-frame

            const [frame, used] = decode(this.inbuf);
            this.inbuf = this.inbuf.slice(used);

            const op = frame.opcode;
            if (op === OP_BIN || op === OP_TEXT || op === OP_CONT) {
                // Every data payload is simply more bytes; _pump runs only
                // once `out` is used up, so this replaces it.
                this.out = frame.payload;
                this.opos = 0;
            } else if (op === OP_PING) {
                // Answering is required (section 5.5.2).
                const [sent, serr, scode] = await this._send(OP_PONG, frame.payload);
                if (!sent) return [null, serr, scode];
            } else if (op === OP_CLOSE) {
                this.closed = true;
                if (!this.closeSent) {
                    this.closeSent = true;
                    try { await this._send(OP_CLOSE, frame.payload); } catch (e) { /* as pcall */ }
                }
            } else if (op !== OP_PONG) {
                // An unsolicited pong is legal and means nothing (5.5.3).
                throw new Error("ssh.ws_stream: unknown opcode " + String(op));
            }
        }
        return [true];
    }

    // -> [true] | [null, message, code]. Returned, not thrown: a write that
    // timed out admitted nothing, so the caller may retry it.
    async _send(opcode, payload) {
        const mask = this.random(4);
        if (typeof mask !== "string" || mask.length !== 4) {
            throw new Error("ssh.ws_stream: random_bytes must return 4 bytes");
        }
        const [ok, err, code] = await this.s.write(encode(opcode, payload, mask));
        if (!ok) return [null, "ssh.ws_stream: write failed: " + String(err), code];
        return [true];
    }

    // The bounded waits of the stream underneath, passed through.
    deadline(ms) {
        if (this.s && this.s.deadline) this.s.deadline(ms);
    }

    wait(ms) {
        if (this.s && this.s.wait) this.s.wait(ms);
    }

    async read(n) {
        if (n <= 0) return [""];
        if (this.opos >= this.out.length) {
            const [ok, err, code] = await this._pump();
            if (!ok) return [null, err, code];
        }
        // Served from an offset rather than by cutting the front off.
        const left = this.out.length - this.opos;
        if (left <= 0) return [""];               // closed, nothing buffered
        const take = n < left ? n : left;
        const s = this.out.slice(this.opos, this.opos + take);
        this.opos += take;
        return [s];
    }

    async write(data) {
        if (data.length === 0) return [true];
        if (this.closed) return [null, "closed"];
        // One binary frame per write.
        let r;
        try {
            r = await this._send(OP_BIN, data);
        } catch (e) {
            // A runtime refusal (a wait inside a transaction, an admission
            // cap) goes on unchanged: as text it read as the connection's
            // io_error (audit 44).
            if (_refusal.code(e) !== null) throw e;
            return [null, errText(e)];
        }
        const [ok, err, code] = r;
        if (!ok) return [null, err, code];
        return [true];
    }

    async close() {
        if (!this.closeSent && !this.closed) {
            this.closeSent = true;
            try { await this._send(OP_CLOSE, u16be(1000)); } catch (e) { /* as pcall */ }
        }
        this.closed = true;
        if (this.s && this.s.close) this.s.close();
    }
}

// Perform the upgrade over `stream` and return a byte stream carrying what is
// inside it. -> [stream] | [null, reason] (reason: { code, detail } or
// { code: "upgrade_refused", status }).
//
//   opts.host     Host header
//   opts.path     default "/"
//   opts.headers  array of "Name: value"
//   opts.random   function(n) -> n random bytes
//   opts.sha1     function(bytes) -> 20 raw digest bytes
async function connect(stream, opts) {
    if (typeof opts.random !== "function" || typeof opts.sha1 !== "function") {
        throw new TypeError("ssh.ws_stream: random and sha1 functions are required");
    }

    const k = key(opts.random);
    const [ok, werr] = await stream.write(buildRequest({
        host: opts.host, path: opts.path, key: k, headers: opts.headers,
    }));
    if (!ok) return [null, { code: "io_error", detail: String(werr) }];

    // Read until the end of the header block, bounded; only the newly read
    // bytes (plus the three before them) are searched.
    const parts = [];
    let total = 0, tail = "", found = false;
    while (!found) {
        if (total > 64 * 1024) {
            return [null, { code: "upgrade_failed", detail: "response headers too large" }];
        }
        const [chunk, err] = await stream.read(4096);
        if (chunk === null || chunk === undefined) {
            return [null, { code: "io_error", detail: String(err) }];
        }
        if (chunk === "") {
            return [null, { code: "upgrade_failed",
                            detail: "connection closed during the upgrade" }];
        }
        parts.push(chunk);
        total += chunk.length;
        const seam = tail + chunk;
        found = seam.indexOf("\r\n\r\n") >= 0;
        tail = seam.slice(-3);
    }
    const buf = parts.join("");

    const [res, perr] = parseResponse(buf);
    if (!res) return [null, { code: "upgrade_failed", detail: perr }];
    if (res.status !== 101) {
        // 403 here is the normal shape of an Access rejection.
        return [null, { code: "upgrade_refused", status: res.status }];
    }
    // RFC 6455 section 4.1: the response must upgrade to exactly websocket.
    if ((res.headers["upgrade"] || "").toLowerCase() !== "websocket") {
        return [null, { code: "upgrade_failed",
                        detail: "the response does not upgrade to websocket" }];
    }
    let upgradeToken = false;
    for (const token of (res.headers["connection"] || "").split(/[,\s]+/)) {
        if (token !== "" && token.toLowerCase() === "upgrade") upgradeToken = true;
    }
    if (!upgradeToken) {
        return [null, { code: "upgrade_failed",
                        detail: "the response's Connection header lacks Upgrade" }];
    }

    const got = res.headers["sec-websocket-accept"];
    if (got !== accept(opts.sha1, k)) {
        return [null, { code: "upgrade_failed",
                        detail: "Sec-WebSocket-Accept does not match" }];
    }

    const rest = buf.slice(buf.indexOf("\r\n\r\n") + 4);
    return [new Stream({
        s:         stream,
        random:    opts.random,
        inbuf:     rest,       // the peer may have framed data immediately
        out:       "",
        opos:      0,          // 0-based (Lua: 1-based)
        readsize:  opts.readsize || 8192,
        closed:    false,
        closeSent: false,
    })];
}

export const wsStream = {
    GUID, OP_CONT, OP_TEXT, OP_BIN, OP_CLOSE, OP_PING, OP_PONG,
    MAX_FRAME, MAX_CONTROL,
    key, accept, buildRequest, parseResponse, applyMask, encode,
    frameSize, decode, connect,
};
export default wsStream;
