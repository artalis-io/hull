// test_ws_stream.js - Tests for hull:ssh:ws_stream (twin of
// stdlib/lua/hull/tests/test_ws_stream.lua).
//
// The handshake and the framing are checked against RFC 6455's own worked
// examples where it gives them.

import { wsStream as ws } from "hull:ssh:ws_stream";
import { encoding } from "hull:encoding";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try { await fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + ((e && e.stack) || e)); }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}

const errText = (e) => (e && typeof e === "object" && typeof e.message === "string")
    ? e.message : String(e);

function assertRaises(fn, msg) {
    let ok = true, err;
    try { fn(); } catch (e) { ok = false; err = e; }
    if (ok) throw new Error((msg || "should have raised") + " but did not");
    return errText(err);
}

const hex = (s) => encoding.hex.encode(s);
const chr = (...a) => String.fromCharCode(...a);
const u16 = (v) => chr((v >>> 8) & 255, v & 255);
function u64(v) {
    const hi = Math.floor(v / 0x100000000), lo = v - hi * 0x100000000;
    return chr((hi >>> 24) & 255, (hi >>> 16) & 255, (hi >>> 8) & 255, hi & 255,
        (lo >>> 24) & 255, (lo >>> 16) & 255, (lo >>> 8) & 255, lo & 255);
}

// handshake ------------------------------------------------------------------

await test("the accept value matches the RFC 6455 worked example", () => {
    const knownKey = "dGhlIHNhbXBsZSBub25jZQ==";
    const raw = encoding.hex.decode("b37a4f2cc0624f1690f64606cf385945b2bec4ea");
    const accept = ws.accept(() => raw, knownKey);
    assertEq(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
});

await test("the key is 16 random bytes, base64 with padding", () => {
    const key = ws.key((n) => "\x00".repeat(n));
    assertEq(key, "AAAAAAAAAAAAAAAAAAAAAA==");
    assertEq(key.length, 24, "16 bytes base64 is 24 characters:");
});

await test("a short random source is refused rather than used", () => {
    assertRaises(() => ws.key(() => "short"), "a 5-byte nonce");
});

await test("the request carries the required upgrade headers", () => {
    const req = ws.buildRequest({ host: "h.example.com", key: "KEY" });
    assertEq(req.indexOf("GET / HTTP/1.1\r\n"), 0);
    assertEq(req.indexOf("Host: h.example.com\r\n") >= 0, true);
    assertEq(req.indexOf("Upgrade: websocket\r\n") >= 0, true);
    assertEq(req.indexOf("Connection: Upgrade\r\n") >= 0, true);
    assertEq(req.indexOf("Sec-WebSocket-Key: KEY\r\n") >= 0, true);
    assertEq(req.indexOf("Sec-WebSocket-Version: 13\r\n") >= 0, true);
    assertEq(req.slice(-4), "\r\n\r\n", "the header block must be terminated:");
});

await test("extra headers are appended in the order given", () => {
    const req = ws.buildRequest({
        host: "h", key: "K",
        headers: ["Cf-Access-Client-Id: abc.access", "Cf-Access-Client-Secret: shh"],
    });
    const a = req.indexOf("Cf-Access-Client-Id: abc.access");
    const b = req.indexOf("Cf-Access-Client-Secret: shh");
    assertEq(a >= 0 && b >= 0 && a < b, true);
});

await test("a header carrying CRLF cannot inject a line", () => {
    assertRaises(() => ws.buildRequest({ host: "h", key: "K",
        headers: ["X: a\r\nInjected: yes"] }), "CRLF in a header");
});

await test("the response head is parsed, names lowercased", () => {
    const [res] = ws.parseResponse(
        "HTTP/1.1 101 Switching Protocols\r\n" +
        "Upgrade: websocket\r\n" +
        "Sec-WebSocket-Accept: abc=\r\n\r\n");
    assertEq(res.status, 101);
    assertEq(res.headers["sec-websocket-accept"], "abc=");
    assertEq(res.headers["upgrade"], "websocket");
});

await test("an incomplete response head is not parsed as one", () => {
    const [res, why] = ws.parseResponse("HTTP/1.1 101 Switching\r\nUpgrade: ws");
    assertEq(res, null);
    assertEq(why, "incomplete");
});

// masking --------------------------------------------------------------------

await test("masking is its own inverse", () => {
    const mask = chr(0x37, 0xfa, 0x21, 0x3d);
    const data = "Hello, this is a longer payload than four bytes";
    assertEq(ws.applyMask(ws.applyMask(data, mask), mask), data);
});

await test("masking matches the RFC 6455 worked example", () => {
    const mask = chr(0x37, 0xfa, 0x21, 0x3d);
    assertEq(hex(ws.applyMask("Hello", mask)), "7f9f4d5158");
});

await test("masking is correct across a chunk boundary", () => {
    const mask = chr(1, 2, 3, 4);
    const data = "z".repeat(5000);
    const out = ws.applyMask(data, mask);
    for (let i = 0; i < data.length; i++) {
        const want = data.charCodeAt(i) ^ mask.charCodeAt(i % 4);
        if (out.charCodeAt(i) !== want) {
            throw new Error("byte " + (i + 1) + " expected " + want + ", got " + out.charCodeAt(i));
        }
    }
});

// framing --------------------------------------------------------------------

await test("a small client frame matches the RFC 6455 worked example", () => {
    const mask = chr(0x37, 0xfa, 0x21, 0x3d);
    assertEq(hex(ws.encode(ws.OP_TEXT, "Hello", mask)), "818537fa213d7f9f4d5158");
});

await test("a client frame always sets the mask bit", () => {
    const f = ws.encode(ws.OP_BIN, "x", "\x00".repeat(4));
    assertEq(f.charCodeAt(1) & 0x80, 0x80, "RFC 6455 5.3: a client MUST mask");
});

await test("the three length encodings are used at the right boundaries", () => {
    const m = "\x00".repeat(4);
    assertEq(ws.encode(ws.OP_BIN, "a".repeat(125), m).charCodeAt(1) & 0x7F, 125);
    assertEq(ws.encode(ws.OP_BIN, "a".repeat(126), m).charCodeAt(1) & 0x7F, 126);
    assertEq(ws.encode(ws.OP_BIN, "a".repeat(65535), m).charCodeAt(1) & 0x7F, 126);
    assertEq(ws.encode(ws.OP_BIN, "a".repeat(65536), m).charCodeAt(1) & 0x7F, 127);
});

await test("encode and decode round trip at each length boundary", () => {
    const m = chr(9, 8, 7, 6);
    for (const n of [0, 1, 125, 126, 127, 65535, 65536, 70000]) {
        const body = "q".repeat(n);
        const f = ws.encode(ws.OP_BIN, body, m);
        let unmasked;
        if (n < 126) unmasked = chr(0x82, n) + body;
        else if (n < 0x10000) unmasked = chr(0x82, 126) + u16(n) + body;
        else unmasked = chr(0x82, 127) + u64(n) + body;
        const [frame, used] = ws.decode(unmasked);
        assertEq(frame !== null && frame.payload, body, "n=" + n + ":");
        assertEq(used, unmasked.length, "n=" + n + " consumed:");
        assertEq(f.length > unmasked.length, true, "the masked form is 4 bytes longer:");
    }
});

await test("a partial frame asks for more rather than guessing", () => {
    const full = chr(0x82, 126) + u16(300) + "z".repeat(300);
    for (const cut of [1, 2, 3, 4, 100, full.length - 1]) {
        const [frame, why] = ws.decode(full.slice(0, cut));
        assertEq(frame, null, "cut=" + cut + ":");
        assertEq(why, "need_more", "cut=" + cut + ":");
    }
    assertEq(ws.decode(full)[0] !== null, true);
});

await test("a masked server frame is refused", () => {
    const f = chr(0x82, 0x81) + "\x00".repeat(4) + "x";
    const err = assertRaises(() => ws.decode(f));
    assertEq(err.indexOf("masked") >= 0, true, err);
});

await test("reserved bits without an extension are refused", () => {
    const err = assertRaises(() => ws.decode(chr(0xC2, 0x01) + "x"));
    assertEq(err.indexOf("reserved") >= 0, true, err);
});

await test("an oversized frame is refused before its bytes are waited for", () => {
    const head = chr(0x82, 127) + u64(2 ** 40);
    const err = assertRaises(() => ws.decode(head));
    assertEq(err.indexOf("exceeds the maximum") >= 0, true, err);
});

await test("a control frame over 125 bytes is refused", () => {
    const head = chr(0x89, 126) + u16(200) + "z".repeat(200);
    const err = assertRaises(() => ws.decode(head));
    assertEq(err.indexOf("125") >= 0, true, err);
});

await test("a fragmented control frame is refused", () => {
    const err = assertRaises(() => ws.decode(chr(0x09, 0x01) + "x"));
    assertEq(err.indexOf("fragmented") >= 0, true, err);
});

await test("encoding an oversized control frame is refused too", () => {
    assertRaises(() => ws.encode(ws.OP_PING, "z".repeat(126), "\x00".repeat(4)), "a 126-byte ping");
});

await test("frame_size reports the whole frame from its header alone", () => {
    assertEq(ws.frameSize(chr(0x82, 5) + "abcde")[0], 7);
    assertEq(ws.frameSize(chr(0x82, 126) + u16(300))[0], 4 + 300);
    assertEq(ws.frameSize(chr(0x82, 127) + u64(70000))[0], 10 + 70000);
});

await test("frame_size needs only the header, not the payload", () => {
    const head = chr(0x82, 126) + u16(9999);
    assertEq(ws.frameSize(head)[0], 4 + 9999, "no payload present:");
    for (const cut of [0, 1, 2, 3]) {
        const [f, why] = ws.frameSize(head.slice(0, cut));
        assertEq(f, null, "cut=" + cut + ":");
        assertEq(why, "need_more", "cut=" + cut + ":");
    }
});

await test("frame_size refuses an absurd length before any bytes are waited for", () => {
    const head = chr(0x82, 127) + u64(2 ** 40);
    assertRaises(() => ws.frameSize(head), "a terabyte frame");
});

// the stream adapter ---------------------------------------------------------

// A fake transport: hands out `inbound` in short pieces, records writes.
function fake(inbound, chunk) {
    return {
        _in: inbound || "", _pos: 0, written: [], closed: false,
        async read(n) {
            if (this._pos >= this._in.length) return [""];
            const take = Math.min(n, chunk || 7, this._in.length - this._pos);
            const s = this._in.slice(this._pos, this._pos + take);
            this._pos += take;
            return [s];
        },
        async write(s) { this.written.push(s); return [true]; },
        close() { this.closed = true; },
    };
}

function serverFrame(opcode, payload) {
    payload = payload || "";
    const n = payload.length;
    if (n < 126) return chr(0x80 | opcode, n) + payload;
    return chr(0x80 | opcode, 126) + u16(n) + payload;
}

const stubRandom = (n) => "\x2a".repeat(n);
const stubSha1 = () => "\x07".repeat(20);
const OPTS = { host: "h", random: stubRandom, sha1: stubSha1 };

function handshakeOk(body) {
    const accept = ws.accept(stubSha1, ws.key(stubRandom));
    return "HTTP/1.1 101 Switching Protocols\r\n"
        + "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        + "Sec-WebSocket-Accept: " + accept + "\r\n\r\n" + (body || "");
}

async function read1(s, n) {
    const [chunk] = await s.read(n);
    return chunk;
}

await test("a successful upgrade yields a byte stream", async () => {
    const t = fake(handshakeOk(serverFrame(ws.OP_BIN, "SSH-2.0-Server\r\n")));
    const [s] = await ws.connect(t, OPTS);
    assertEq(s !== null, true);
    assertEq(await read1(s, 64), "SSH-2.0-Server\r\n");
});

await test("a non-101 status is reported with the status", async () => {
    const t = fake("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
    const [s, err] = await ws.connect(t, OPTS);
    assertEq(s, null);
    assertEq(err.code, "upgrade_refused");
    assertEq(err.status, 403);
});

await test("a wrong Sec-WebSocket-Accept is refused", async () => {
    const t = fake("HTTP/1.1 101 Switching Protocols\r\n"
        + "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        + "Sec-WebSocket-Accept: wrong\r\n\r\n");
    const [s, err] = await ws.connect(t, OPTS);
    assertEq(s, null);
    assertEq(err.code, "upgrade_failed");
    assertEq(err.detail, "Sec-WebSocket-Accept does not match");
});

// A 101 with a correct accept value, and the Upgrade / Connection lines given.
function responseWith(lines) {
    const accept = ws.accept(stubSha1, ws.key(stubRandom));
    return "HTTP/1.1 101 Switching Protocols\r\n" + lines
        + "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
}

await test("a 101 that does not upgrade to websocket is refused", async () => {
    for (const lines of ["Connection: Upgrade\r\n",
                         "Upgrade: h2c\r\nConnection: Upgrade\r\n"]) {
        const [s, err] = await ws.connect(fake(responseWith(lines)), OPTS);
        assertEq(s, null, lines);
        assertEq(err.detail, "the response does not upgrade to websocket");
    }
});

await test("a 101 whose Connection lacks Upgrade is refused", async () => {
    const [s, err] = await ws.connect(
        fake(responseWith("Upgrade: websocket\r\nConnection: keep-alive\r\n")), OPTS);
    assertEq(s, null);
    assertEq(err.detail, "the response's Connection header lacks Upgrade");
});

await test("Upgrade among other Connection tokens, in any case, is accepted", async () => {
    const [s] = await ws.connect(
        fake(responseWith("upgrade: WebSocket\r\nConnection: keep-alive, UPGRADE\r\n")), OPTS);
    assertEq(s !== null, true);
});

await test("a NUL in a request header is refused", async () => {
    let ok = true, err;
    try {
        await ws.connect(fake(""), { host: "h", random: stubRandom, sha1: stubSha1,
            headers: ["X-Id: a\x00b"] });
    } catch (e) { ok = false; err = e; }
    assertEq(ok, false);
    assertEq(errText(err).indexOf("NUL") >= 0, true, errText(err));
});

await test("a connection closed mid-upgrade is an error, not a hang", async () => {
    const [s, err] = await ws.connect(fake("HTTP/1.1 101 Switch"), OPTS);
    assertEq(s, null);
    assertEq(err.code, "upgrade_failed");
});

await test("frames arriving with the handshake are not lost", async () => {
    const [s] = await ws.connect(fake(handshakeOk(serverFrame(ws.OP_BIN, "immediate"))), OPTS);
    assertEq(await read1(s, 32), "immediate");
});

await test("fragmentation needs no reassembly for a byte stream", async () => {
    const body = chr(0x00 | ws.OP_BIN, 3) + "abc"      // FIN clear
        + chr(0x80 | ws.OP_CONT, 3) + "def";            // FIN set
    const [s] = await ws.connect(fake(handshakeOk(body)), OPTS);
    assertEq(await read1(s, 64), "abc");
    assertEq(await read1(s, 64), "def");
});

await test("a short read returns what is there rather than waiting", async () => {
    const [s] = await ws.connect(fake(handshakeOk(serverFrame(ws.OP_BIN, "abcdef"))), OPTS);
    assertEq(await read1(s, 3), "abc");
    assertEq(await read1(s, 3), "def");
});

await test("a ping is answered with a pong carrying the same payload", async () => {
    const t = fake(handshakeOk(serverFrame(ws.OP_PING, "beat") + serverFrame(ws.OP_BIN, "data")));
    const [s] = await ws.connect(t, OPTS);
    assertEq(await read1(s, 16), "data", "the ping must not surface as data:");

    let pong = null;
    for (let i = 1; i < t.written.length; i++) {
        const w = t.written[i];
        if ((w.charCodeAt(0) & 0x0F) === ws.OP_PONG) pong = w;
    }
    assertEq(pong !== null, true, "a PONG should have been sent");
    assertEq(ws.applyMask(pong.slice(6), pong.slice(2, 6)), "beat");
});

await test("a close frame ends the stream", async () => {
    const t = fake(handshakeOk(serverFrame(ws.OP_BIN, "last") + serverFrame(ws.OP_CLOSE, u16(1000))));
    const [s] = await ws.connect(t, OPTS);
    assertEq(await read1(s, 16), "last");
    assertEq(await read1(s, 16), "", "EOF after close:");
});

await test("a transport that ends without a close frame is EOF, not an error", async () => {
    const [s] = await ws.connect(fake(handshakeOk(serverFrame(ws.OP_BIN, "partial"))), OPTS);
    assertEq(await read1(s, 16), "partial");
    assertEq(await read1(s, 16), "");
});

await test("writes are masked binary frames", async () => {
    const t = fake(handshakeOk());
    const [s] = await ws.connect(t, OPTS);
    const [ok] = await s.write("payload");
    assertEq(ok, true);
    const last = t.written[t.written.length - 1];
    assertEq(last.charCodeAt(0) & 0x0F, ws.OP_BIN);
    assertEq(last.charCodeAt(1) & 0x80, 0x80, "client frames are masked:");
    const mask = last.slice(2, 6);
    assertEq(ws.applyMask(last.slice(6), mask), "payload");
});

await test("close sends a close frame and closes the transport", async () => {
    const t = fake(handshakeOk());
    const [s] = await ws.connect(t, OPTS);
    await s.close();
    const last = t.written[t.written.length - 1];
    assertEq(last.charCodeAt(0) & 0x0F, ws.OP_CLOSE);
    assertEq(t.closed, true);
});

await test("a frame dribbled out one byte at a time still assembles", async () => {
    const body = "z".repeat(900);
    const [s] = await ws.connect(fake(handshakeOk(serverFrame(ws.OP_BIN, body)), 1), OPTS);
    const got = [];
    for (;;) {
        const piece = await read1(s, 4096);
        if (piece === "") break;
        got.push(piece);
    }
    assertEq(got.join(""), body);
});

await test("a newline in the path or the host is refused", () => {
    assertRaises(() => ws.buildRequest({ host: "relay", key: "k",
        path: "/x\r\nCf-Access-Jump-Destination: evil:22" }), "CRLF in path");
    assertRaises(() => ws.buildRequest({ host: "relay\r\nX-Forged: 1", key: "k" }), "CRLF in host");
    assertRaises(() => ws.buildRequest({ host: "relay", key: "k\r\nX: 1" }), "CRLF in key");
    const req = ws.buildRequest({ host: "relay", key: "k", path: "/ok" });
    assertEq(req.indexOf("GET /ok HTTP/1.1") >= 0, true);
});

await test("small reads walk one payload in order, then the next frame", async () => {
    const parts = [];
    for (let i = 1; i <= 300; i++) parts.push(chr(i % 256));
    const payload = parts.join("");
    const t = fake(handshakeOk(serverFrame(ws.OP_BIN, payload) + serverFrame(ws.OP_BIN, "tail")), 64);
    const [s] = await ws.connect(t, OPTS);
    const got = [];
    for (let i = 0; i < 100; i++) got.push(await read1(s, 3));
    assertEq(got.join(""), payload);
    assertEq(await read1(s, 2), "ta");
    assertEq(await read1(s, 64), "il");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
