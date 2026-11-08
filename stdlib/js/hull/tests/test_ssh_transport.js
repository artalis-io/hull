// test_ssh_transport.js - Tests for hull:ssh:transport
//
// The JS twin of test_ssh_transport.lua. Drives the transport against a fake
// stream, so the SEQUENCE can be tested without a server: that a banner
// before the identification line is skipped, that transport chatter is
// filtered, that a global request gets refused rather than ignored, that a
// disconnect surfaces as an error rather than a hang, and that an unknown
// host stops the connection with a fingerprint instead of proceeding.

import { transport } from "hull:ssh:transport";
import { ssh } from "hull:ssh";
import { packet } from "hull:ssh:packet";
import { kexinit } from "hull:ssh:kexinit";
import { wire } from "hull:ssh:wire";
import { cipher } from "hull:ssh:cipher";
import { channel } from "hull:ssh:channel";
import { sftp as sftpCodec } from "hull:ssh:sftp";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try {
        await fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + ((e && e.stack) || errText(e)));
    }
}

const errText = (e) => (e && typeof e === "object" && typeof e.message === "string")
    ? e.message : String(e);

function assertEq(a, b, msg) {
    if (a !== b) {
        throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
    }
}

function assertOk(v, msg) {
    if (!v) throw new Error(msg || "assertion failed");
    return v;
}

async function assertRaises(fn, msg) {
    try {
        await fn();
    } catch (e) {
        return errText(e);
    }
    throw new Error((msg || "should have raised") + " but did not");
}

const has = (s, sub) => s.indexOf(sub) >= 0;
const zeros = (n) => "\x00".repeat(n);
const u32 = (v) => wire.uint32(v);
const b = (n) => String.fromCharCode(n);

// A stream over a fixed script of inbound bytes, recording what was written.
// read() deliberately returns SHORT chunks, because the real binding does.
function fakeStream(inbound, chunk) {
    return {
        _in: inbound, _pos: 0, written: [], closed: false,
        read(n) {
            if (this._pos >= this._in.length) return [""];
            const take = Math.min(n, chunk || 3, this._in.length - this._pos);
            const s = this._in.slice(this._pos, this._pos + take);
            this._pos = this._pos + take;
            return [s];
        },
        write(s) {
            this.written.push(s);
            return [true];
        },
        close() { this.closed = true; },
    };
}

function stubCrypto() {
    return {
        random: (n) => zeros(n),
        sha256: () => "\xab".repeat(32),
    };
}

function plain(payload) {
    return packet.frame(payload, 8, (n) => zeros(n));
}

// reading -------------------------------------------------------------------

await test("short reads are assembled, not treated as failure", async () => {
    const t = transport.new(fakeStream("abcdefghij", 2), stubCrypto());
    await t.fill(10);
    assertEq(t.inbuf, "abcdefghij");
});

await test("a closed stream mid-packet is an error, not a hang", async () => {
    const t = transport.new(fakeStream("abc"), stubCrypto());
    const err = await assertRaises(() => t.fill(10));
    assertEq(has(err, "closed"), true, err);
});

// A stream that records the largest read it was asked for.
function askingStream(inbound) {
    const s = fakeStream(inbound, 4096);
    s.maxAsked = 0;
    const read = s.read;
    s.read = function (n) {
        if (n > this.maxAsked) this.maxAsked = n;
        return read.call(this, n);
    };
    return s;
}

await test("a huge declared length is refused before anything waits for it", async () => {
    const s = askingStream(u32(0xFFFFFFFF) + "rest");
    const t = transport.new(s, stubCrypto());
    const err = await assertRaises(() => t.readPacket());
    assertEq(has(err, "exceeds the maximum"), true, err);
    assertEq(s.maxAsked <= 32768, true, "asked the stream for " + String(s.maxAsked) + " bytes");
});

await test("a length below the minimum is refused, not waited for", async () => {
    const s = askingStream(u32(2) + "xx");
    const t = transport.new(s, stubCrypto());
    const err = await assertRaises(() => t.readPacket());
    assertEq(has(err, "below the minimum"), true, err);
});

await test("a misaligned length is refused, not waited for", async () => {
    const s = askingStream(u32(13));
    const t = transport.new(s, stubCrypto());
    const err = await assertRaises(() => t.readPacket());
    assertEq(has(err, "block size"), true, err);
});

await test("after NEWKEYS a huge declared length is refused the same way", async () => {
    const s = askingStream(u32(0xFFFFFFF0) + "rest");
    const t = transport.new(s, stubCrypto());
    t.s2c = cipher.new("k".repeat(32), "i".repeat(12));
    const err = await assertRaises(() => t.readPacket());
    assertEq(has(err, "exceeds the maximum"), true, err);
    assertEq(s.maxAsked <= 32768, true, "asked the stream for " + String(s.maxAsked) + " bytes");
});

await test("a valid packet still arrives whole through short reads", async () => {
    const t = transport.new(fakeStream(plain(b(20) + "real"), 3), stubCrypto());
    const p = await t.readPacket();
    assertEq(p, b(20) + "real");
    assertEq(t.inbuf, "");
});

await test("identification lines are read one at a time", async () => {
    const t = transport.new(fakeStream("first\r\nsecond\r\n"), stubCrypto());
    assertEq(await t.readLine(), "first");
    assertEq(await t.readLine(), "second");
});

await test("a peer that never sends a newline is bounded", async () => {
    const t = transport.new(fakeStream("x".repeat(4000)), stubCrypto());
    await assertRaises(() => t.readLine(), "unbounded line");
});

// message filtering ------------------------------------------------------------

await test("IGNORE and DEBUG are skipped", async () => {
    const inbound = plain(b(2) + "ignore me")
        + plain(b(4) + "\x00debug")
        + plain(b(20) + "real");
    const t = transport.new(fakeStream(inbound), stubCrypto());
    assertEq((await t.nextMessage()).charCodeAt(0), 20);
});

await test("a global request wanting a reply is refused, not ignored", async () => {
    const gr = wire.writer().byte(80).string("keepalive@openssh.com").boolean(true).build();
    const inbound = plain(gr) + plain(b(20) + "real");
    const s = fakeStream(inbound);
    const t = transport.new(s, stubCrypto());
    assertEq((await t.nextMessage()).charCodeAt(0), 20);

    const sent = s.written.join("");
    const [reply] = packet.parse(sent, 8);
    assertEq(reply.charCodeAt(0), 82, "REQUEST_FAILURE");
});

await test("a global request not wanting a reply gets none", async () => {
    const gr = wire.writer().byte(80).string("x").boolean(false).build();
    const s = fakeStream(plain(gr) + plain(b(20) + "real"));
    const t = transport.new(s, stubCrypto());
    await t.nextMessage();
    assertEq(s.written.length, 0, "nothing should have been sent");
});

await test("DISCONNECT surfaces the server reason", async () => {
    const d = wire.writer().byte(1).uint32(11)
        .string("too many authentication failures").string("en").build();
    const t = transport.new(fakeStream(plain(d)), stubCrypto());
    const err = await assertRaises(() => t.nextMessage());
    assertEq(has(err, "too many authentication"), true, err);
    assertEq(has(err, "11"), true, err);
});

await test("a DISCONNECT reason cannot add a line to the caller's log", async () => {
    const d = wire.writer().byte(1).uint32(11)
        .string("bye" + b(10) + "host-b: OK").string("en").build();
    const t = transport.new(fakeStream(plain(d)), stubCrypto());
    const err = await assertRaises(() => t.nextMessage());
    assertEq(has(err, b(10)), false, err);
    assertEq(has(err, "byehost-b: OK"), true, err);
});

await test("a hostile disconnect message cannot write escapes to a terminal", async () => {
    const d = wire.writer().byte(1).uint32(2).string("bye\x1b[2Jcleared").string("").build();
    const t = transport.new(fakeStream(plain(d)), stubCrypto());
    const err = await assertRaises(() => t.nextMessage());
    assertEq(has(err, "\x1b"), false, "escape survived");
});

await test("endless chatter does not loop forever", async () => {
    const one = plain(b(2) + "ignore");
    const t = transport.new(fakeStream(one.repeat(400)), stubCrypto());
    await assertRaises(() => t.nextMessage(), "chatter bound");
});

// handshake refusals ----------------------------------------------------------------

await test("a server with no identification line does not connect", async () => {
    const t = transport.new(fakeStream("hello\r\nthere\r\n"), stubCrypto());
    let ok = true, res = null;
    try {
        res = await t.handshake({ host: "h", trust: {} });
    } catch (e) {
        ok = false;
    }
    assertEq(ok === false || res[0] === null || res[0] === undefined, true,
             "must not report success");
});

await test("banners before the identification line are reported", async () => {
    const seen = [];
    const inbound = "### maintenance window ###\r\nSSH-2.0-Test\r\n";
    const t = transport.new(fakeStream(inbound), stubCrypto());
    try {
        await t.handshake({ host: "h", trust: {}, onBanner: (x) => { seen.push(x); } });
    } catch (e) { /* the stream ends */ }
    assertEq(seen.length, 1);
    assertEq(seen[0], "### maintenance window ###");
});

await test("a banner cannot smuggle terminal escapes", async () => {
    const seen = [];
    const inbound = "evil\x1b[2J\r\nSSH-2.0-Test\r\n";
    const t = transport.new(fakeStream(inbound), stubCrypto());
    try {
        await t.handshake({ host: "h", trust: {}, onBanner: (x) => { seen.push(x); } });
    } catch (e) { /* the stream ends */ }
    assertEq(has(seen[0], "\x1b"), false, seen[0]);
});

await test("no common algorithm is a structured refusal", async () => {
    const server = kexinit.build({
        kex: ["diffie-hellman-group1-sha1"],
        hostKey: ["ssh-rsa"],
        cipher: ["3des-cbc"],
        mac: ["hmac-md5"],
        compression: ["none"],
        languages: [],
    }, zeros(16));

    const inbound = "SSH-2.0-Test\r\n" + plain(server);
    const t = transport.new(fakeStream(inbound), stubCrypto());
    const [ok, err] = await t.handshake({ host: "h", trust: {} });
    assertEq(ok, null);
    assertEq(err.code, "no_common_algorithm");
    assertEq(has(err.detail, "key exchange"), true, err.detail);
});

// A whole key exchange against a scripted server, with every primitive
// stubbed and each verify recorded.
function fullCrypto(calls) {
    const c = stubCrypto();
    c.x25519Keypair = () => ["\x09".repeat(32), "\x01".repeat(32)];
    c.x25519 = () => ["\x02".repeat(32)];
    c.gcmSeal = (_k, _iv, _aad, p) => [p, zeros(16)];
    c.gcmOpen = (_k, _iv, _aad, x) => x;
    c.ed25519Verify = () => { calls.push("ed25519"); return true; };
    c.verify = (jose) => { calls.push(jose); return true; };
    return c;
}

const P256 = wire.writer().string("ecdsa-sha2-nistp256").string("nistp256")
    .string("\x04" + "\x05".repeat(64)).build();
const ED = wire.writer().string("ssh-ed25519").string("\x06".repeat(32)).build();

function ecdsaSig() {
    const rs = wire.writer().mpint("\x01".repeat(32)).mpint("\x02".repeat(32)).build();
    return wire.writer().string("ecdsa-sha2-nistp256").string(rs).build();
}

function offerWith(fields) {
    return Object.assign({}, kexinit.DEFAULT_OFFER, fields);
}

function serverOffering(hostKeys, key, sig) {
    const offer = offerWith({ hostKey: hostKeys });
    return "SSH-2.0-Test\r\n" + plain(kexinit.build(offer, zeros(16)))
        + plain(wire.writer().byte(31).string(key).string("Q".repeat(32)).string(sig).build())
        + plain("\x15");
}

await test("a server with only an ECDSA host key is verified with ES256", async () => {
    const calls = [];
    const s = fakeStream(serverOffering(["ecdsa-sha2-nistp256"], P256, ecdsaSig()), 64);
    const t = transport.new(s, fullCrypto(calls));
    const [ok, err] = await t.handshake({ host: "h", trust: { get: () => P256 } });
    assertEq(ok, true, err && err.code);
    assertEq(calls.length, 1);
    assertEq(calls[0], "ES256");
    assertEq(t.negotiated.hostKey, "ecdsa-sha2-nistp256");
});

await test("a key of another type than the negotiated algorithm is refused", async () => {
    const calls = [];
    const s = fakeStream(serverOffering(["ecdsa-sha2-nistp256"], ED, ecdsaSig()), 64);
    const t = transport.new(s, fullCrypto(calls));
    const [ok, err] = await t.handshake({ host: "h", trust: { get: () => ED } });
    assertEq(ok, null);
    assertEq(err.code, "host_key_invalid");
    assertEq(calls.length, 0, "nothing reached a verifier:");
});

// A rekey that presents a DIFFERENT host key: the whole exchange runs.
function edKey(byte) {
    return wire.writer().string("ssh-ed25519").string(byte.repeat(32)).build();
}
function edSig() {
    return wire.writer().string("ssh-ed25519").string("\x08".repeat(64)).build();
}

// Packets after NEWKEYS, under the stub's identity AEAD.
function sealed(payloads) {
    const c = cipher.new(zeros(32), zeros(12));
    const aead = { seal: (_k, _iv, _aad, p) => [p, zeros(16)] };
    const out = [];
    for (const p of payloads) out.push(c.seal(aead, p, (n) => zeros(n)));
    return out.join("");
}

await test("a rekey presenting another host key is refused as host_changed_midsession", async () => {
    const KEY_A = edKey("A"), KEY_B = edKey("B");
    const offer = offerWith({ hostKey: ["ssh-ed25519"] });
    const serverKexinit = kexinit.build(offer, zeros(16));
    const reply = (key) => wire.writer().byte(31).string(key).string("Q".repeat(32))
        .string(edSig()).build();
    const inbound = "SSH-2.0-Test\r\n" + plain(serverKexinit) + plain(reply(KEY_A))
        + plain("\x15")
        + sealed([serverKexinit, reply(KEY_B)]);
    const calls = [];
    const c = fullCrypto(calls);
    // A digest that depends on its input, so the two keys' fingerprints differ.
    c.sha256 = (d) => (zeros(32) + d).slice(-32);
    const t = transport.new(fakeStream(inbound, 64), c);
    const [ok, err] = await t.handshake({ host: "h", trust: { get: () => KEY_A } });
    assertEq(ok, true, err && err.code);

    const [rok, rerr] = await t.rekey();
    assertEq(rok, null);
    assertEq(rerr.code, "host_changed_midsession");
    assertEq(typeof rerr.fingerprint === "string" && rerr.fingerprint !== rerr.storedFingerprint,
             true, "fingerprints:");
    assertEq(calls.length, 1, "only the first exchange verified a signature:");
    assertEq(t.dead !== null && t.dead !== undefined, true,
             "a failed rekey leaves the connection usable:");
    const [och, oerr] = await t.openSession();
    assertEq(och, null);
    assertEq(oerr.code, "host_changed_midsession");
    await assertRaises(() => t.sendPacket("\x02"), "send after a failed rekey");
});

await test("a pending trust lets an unknown host through, holds it, and pins the rekey", async () => {
    const KEY_A = edKey("A"), KEY_B = edKey("B");
    const offer = offerWith({ hostKey: ["ssh-ed25519"] });
    const serverKexinit = kexinit.build(offer, zeros(16));
    const reply = (key) => wire.writer().byte(31).string(key).string("Q".repeat(32))
        .string(edSig()).build();
    const inbound = "SSH-2.0-Test\r\n" + plain(serverKexinit) + plain(reply(KEY_A))
        + plain("\x15")
        + sealed([serverKexinit, reply(KEY_B)]);
    const calls = [];
    const c = fullCrypto(calls);
    c.sha256 = (d) => (zeros(32) + d).slice(-32);
    const pending = ssh.pendingTrust();
    const t = transport.new(fakeStream(inbound, 64), c);
    const [ok, err] = await t.handshake({ host: "h", trust: pending });
    assertEq(ok, true, err && err.code);
    assertEq(t.hostKeyBlob, KEY_A, "the presented key is the connection's:");
    assertEq(typeof t.sessionId === "string" && t.sessionId.length === 32, true, "session id:");
    assertEq(pending.get("h"), KEY_A, "the pending trust holds it:");
    assertEq(t.kexOpts.trust, pending, "kept for conn.trustPending():");

    const [rok, rerr] = await t.rekey();
    assertEq(rok, null);
    assertEq(rerr.code, "host_changed_midsession");
});

// The host key algorithms in the first KEXINIT this client sent.
function offeredHostKeys(s) {
    for (const w of s.written) {
        const payload = w.slice(0, 4) !== "SSH-" && packet.parse(w, 8)[0];
        if (payload && payload.charCodeAt(0) === 20) {
            return kexinit.parse(payload).hostKey.join(",");
        }
    }
    return undefined;
}

await test("the key type the trust store holds for a host is asked for first", async () => {
    const rsa = wire.writer().string("ssh-rsa").string("e").string("n").build();
    const s = fakeStream("SSH-2.0-Test\r\n");
    const t = transport.new(s, stubCrypto());
    try {
        await t.handshake({ host: "h", trust: { get: () => rsa } });
    } catch (e) { /* the stream ends */ }
    assertEq(offeredHostKeys(s),
             "rsa-sha2-512,rsa-sha2-256,ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384");
});

await test("with nothing stored the default host key order is offered", async () => {
    const s = fakeStream("SSH-2.0-Test\r\n");
    const t = transport.new(s, stubCrypto());
    try { await t.handshake({ host: "h", trust: { get: () => undefined } }); } catch (e) { /* */ }
    assertEq(offeredHostKeys(s), kexinit.DEFAULT_OFFER.hostKey.join(","));
});

await test("a trust store that fails to read leaves the offer alone", async () => {
    const s = fakeStream("SSH-2.0-Test\r\n");
    const t = transport.new(s, stubCrypto());
    try {
        await t.handshake({ host: "h", trust: { get: () => { throw new Error("disk gone"); } } });
    } catch (e) { /* */ }
    assertEq(offeredHostKeys(s), kexinit.DEFAULT_OFFER.hostKey.join(","));
});

await test("the client identification string is sent after the server one", async () => {
    const s = fakeStream("SSH-2.0-Test\r\n");
    const t = transport.new(s, stubCrypto(), { software: "Hull_x" });
    try { await t.handshake({ host: "h", trust: {} }); } catch (e) { /* */ }
    assertEq(s.written[0], "SSH-2.0-Hull_x\r\n");
});

// exec ----------------------------------------------------------------------

function conf(window, maxp) {
    return plain(wire.writer().byte(91).uint32(0).uint32(7)
        .uint32(window || 65536).uint32(maxp || 32768).build());
}
const okReply = () => plain(wire.writer().byte(99).uint32(0).build());
const data = (s) => plain(wire.writer().byte(94).uint32(0).string(s).build());
const errdata = (s) => plain(wire.writer().byte(95).uint32(0).uint32(1).string(s).build());
const status = (code) => plain(wire.writer().byte(98).uint32(0).string("exit-status")
    .boolean(false).uint32(code).build());
const grant = (n) => plain(wire.writer().byte(93).uint32(0).uint32(n).build());
const eof = () => plain(wire.writer().byte(96).uint32(0).build());
const fin = () => plain(wire.writer().byte(97).uint32(0).build());

// The message type of a packet we wrote: 4 length, 1 padding length, then the
// payload, whose first byte is the type.
function writtenTypes(s) {
    const types = [];
    for (const p of s.written) {
        if (p.length >= 6) types.push(p.charCodeAt(5));
    }
    return types;
}

function hasType(s, want) {
    for (const ty of writtenTypes(s)) if (ty === want) return true;
    return false;
}

await test("exec streams stdout to a callback as it arrives", async () => {
    const s = fakeStream(conf() + okReply() + data("one\n") + data("two\n")
        + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const chunks = [];
    const [r] = await t.exec("cmd", { onStdout: (c) => { chunks.push(c); } });
    assertEq(chunks.length, 2, "one callback per inbound chunk:");
    assertEq(chunks[0], "one\n");
    assertEq(chunks[1], "two\n");
    assertEq(r.stdout, "", "streamed stdout must not be buffered too:");
    assertEq(r.status, 0);
});

await test("a streamed command is not bounded by max_output", async () => {
    const big = "x".repeat(4096);
    const s = fakeStream(conf() + okReply() + data(big) + data(big) + data(big)
        + status(0) + eof() + fin(), 64);
    const t = transport.new(s, stubCrypto());
    let seen = 0;
    const [r] = await t.exec("cmd", { maxOutput: 16, onStdout: (c) => { seen += c.length; } });
    assertEq(seen, 3 * 4096);
    assertEq(r.status, 0);
});

await test("without a callback the output cap still applies", async () => {
    const big = "x".repeat(4096);
    const s = fakeStream(conf() + okReply() + data(big) + data(big)
        + status(0) + eof() + fin(), 64);
    const t = transport.new(s, stubCrypto());
    const [r, err] = await t.exec("cmd", { maxOutput: 16 });
    assertEq(r, null);
    assertEq(err.code, "output_too_large");
    assertEq(hasType(s, 97), true, "an aborted exec must close its channel:");
});

await test("maxOutput 0 is a 0-byte cap; a non-integer is refused (#1009 review)", async () => {
    const s = fakeStream(conf() + okReply() + data("x") + status(0) + eof() + fin(), 64);
    const t = transport.new(s, stubCrypto());
    const [r, err] = await t.exec("cmd", { maxOutput: 0 });
    assertEq(r, null);
    assertEq(err.code, "output_too_large");
    for (const bad of [NaN, "16", -1, 1.5, {}]) {
        const t2 = transport.new(fakeStream("", 64), stubCrypto());
        const [r2, err2] = await t2.exec("cmd", { maxOutput: bad });
        assertEq(r2, null);
        assertEq(err2 && err2.code, "bad_argument", String(bad));
    }
});

await test("streaming one stream still accumulates the other", async () => {
    const s = fakeStream(conf() + okReply() + data("out") + errdata("err")
        + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const got = [];
    const [r] = await t.exec("cmd", { onStdout: (c) => { got.push(c); } });
    assertEq(got[0], "out");
    assertEq(r.stdout, "");
    assertEq(r.stderr, "err", "stderr has no callback, so it is buffered:");
});

await test("a channel request wanting a reply is answered, not ignored", async () => {
    const keepalive = plain(wire.writer().byte(98).uint32(0)
        .string("keepalive@openssh.com").boolean(true).build());
    const s = fakeStream(conf() + okReply() + data("a") + keepalive
        + data("b") + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const [r] = await t.exec("cmd");
    assertEq(r.stdout, "ab", "the command carried on:");
    assertEq(r.status, 0);
    assertEq(hasType(s, 100), true, "CHANNEL_FAILURE sent:");
});

await test("a channel request not wanting a reply gets none", async () => {
    const s = fakeStream(conf() + okReply() + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    await t.exec("cmd");
    assertEq(hasType(s, 100), false, "no CHANNEL_FAILURE:");
});

await test("a channel the server asks to open is refused with a reply", async () => {
    const open = plain(wire.writer().byte(90).string("auth-agent@openssh.com")
        .uint32(5).uint32(65536).uint32(32768).build());
    const s = fakeStream(conf() + okReply() + open + data("x")
        + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const [r] = await t.exec("cmd");
    assertEq(r.stdout, "x");
    assertEq(hasType(s, 92), true, "CHANNEL_OPEN_FAILURE sent:");
});

await test("stdin is written to the command and followed by EOF", async () => {
    const s = fakeStream(conf() + okReply() + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const [r] = await t.exec("cat", { stdin: "hello" });
    assertEq(r.status, 0);
    assertEq(hasType(s, 94), true, "stdin must be sent as CHANNEL_DATA:");
    assertEq(hasType(s, 96), true, "stdin must be terminated with CHANNEL_EOF:");
    const joined = s.written.join("");
    assertEq(has(joined, "hello"), true);
});

await test("stdin larger than the window waits for the peer to grant more", async () => {
    const s = fakeStream(conf(4, 4) + okReply() + grant(4)
        + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const [r] = await t.exec("cat", { stdin: "abcdefgh" });
    assertEq(r.status, 0);
    const sent = [];
    for (const p of s.written) {
        if (p.length >= 6 && p.charCodeAt(5) === 94) sent.push(p.slice(10));
    }
    assertEq(sent.length, 2, "eight bytes through a four byte window is two writes:");
});

await test("a stdout callback that raises closes the channel", async () => {
    const s = fakeStream(conf() + okReply() + data("boom")
        + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    await assertRaises(() => t.exec("cmd", { onStdout: () => { throw new Error("caller blew up"); } }),
                       "a raising callback should propagate");
    assertEq(hasType(s, 97), true, "the channel must not be left half open behind it:");
});

await test("an oversized stdin is refused up front, not part way through", async () => {
    const s = fakeStream(conf(4 * 1024 * 1024, 32768) + okReply()
        + status(0) + eof() + fin(), 4096);
    const t = transport.new(s, stubCrypto());
    const [r, err] = await t.exec("cat", { stdin: "y".repeat(512 * 1024) });
    assertEq(r, null);
    assertEq(err.code, "stdin_too_large");
    assertEq(err.limit, 128 * 1024);
    assertEq(hasType(s, 94), false, "and nothing was written to the wire:");
    assertEq(hasType(s, 97), true, "but the channel is still closed:");
});

await test("a stdin at the limit is still accepted", async () => {
    const s = fakeStream(conf(4 * 1024 * 1024, 32768) + okReply()
        + status(0) + eof() + fin(), 4096);
    const t = transport.new(s, stubCrypto());
    const [r] = await t.exec("cat", { stdin: "y".repeat(128 * 1024) });
    assertEq(r !== null && r !== undefined && r.status, 0);
    assertEq(hasType(s, 94), true, "it reached the wire:");
    assertEq(hasType(s, 96), true, "and was terminated with EOF:");
});

await test("a non-string stdin is refused rather than coerced", async () => {
    const s = fakeStream(conf() + okReply() + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const [r, err] = await t.exec("cat", { stdin: 42 });
    assertEq(r, null);
    assertEq(err.code, "bad_stdin");
});

// stdin from a source function ----------------------------------------------

// The bytes of every CHANNEL_DATA we wrote, joined.
function writtenData(s) {
    const out = [];
    for (const p of s.written) {
        if (p.length >= 6 && p.charCodeAt(5) === 94) {
            const r = wire.reader(packet.parse(p, 8)[0]);
            r.byte(); r.uint32();
            out.push(r.string());
        }
    }
    return out.join("");
}

// A source serving `chunks` in order, then EOF.
function source(chunks) {
    let i = 0;
    return () => chunks[i++];
}

await test("a stdin source is streamed past the string cap, then EOF", async () => {
    const big = "z".repeat(100 * 1024);
    const s = fakeStream(conf(4 * 1024 * 1024, 32768) + okReply()
        + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const [r] = await t.exec("cat", { stdin: source([big, big, big]) });
    assertEq(r !== null && r !== undefined && r.status, 0);
    assertEq(writtenData(s).length, 3 * 100 * 1024, "all 300 KiB reached the wire:");
    assertEq(hasType(s, 96), true, "and was terminated with EOF:");
});

await test("a stdin source respects the peer's window", async () => {
    const s = fakeStream(conf(4, 4) + okReply() + grant(4) + grant(4)
        + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const [r] = await t.exec("cat", { stdin: source(["abcdef", "ghij"]) });
    assertEq(r.status, 0);
    assertEq(writtenData(s), "abcdefghij");
});

// A stream that answers a short read with what it has.
function waitingStream(inbound) {
    const st = fakeStream(inbound, 65536);
    st.wait = () => {};
    return st;
}

await test("output and the close that arrive while stdin streams are taken in", async () => {
    const s = waitingStream(conf(4 * 1024 * 1024, 32768) + okReply()
        + data("early") + status(0) + eof() + fin());
    const t = transport.new(s, stubCrypto());
    const got = [];
    const [r] = await t.exec("cat", {
        stdin: source(["one", "two", "three"]),
        onStdout: (c) => { got.push(c); },
    });
    assertEq(r !== null && r !== undefined && r.status, 0);
    assertEq(got.join(""), "early");
});

await test("a stdin source that raises is the caller's error, and the channel closes", async () => {
    const s = fakeStream(conf() + okReply() + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    await assertRaises(() => t.exec("cat", { stdin: () => { throw new Error("source blew up"); } }),
                       "a raising source should propagate");
    assertEq(hasType(s, 97), true, "the channel must not be left half open:");
});

await test("a stdin source returning a non-string is bad_stdin", async () => {
    const s = fakeStream(conf() + okReply() + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const [r, err] = await t.exec("cat", { stdin: source(["ok", 42]) });
    assertEq(r, null);
    assertEq(err.code, "bad_stdin");
});

// KEX message discipline ------------------------------------------------------

await test("the expected message is required, not merely looked for", async () => {
    const s = fakeStream(plain(b(30) + "wrong"), 4);
    const t = transport.new(s, stubCrypto());
    const err = await assertRaises(() => t.expect(21, "NEWKEYS"));
    assertEq(has(err, "expected NEWKEYS"), true, err);
    assertEq(has(err, "message 30"), true, err);
});

await test("the expected message passes through", async () => {
    const s = fakeStream(plain(b(21)), 4);
    const t = transport.new(s, stubCrypto());
    assertEq((await t.expect(21, "NEWKEYS")).charCodeAt(0), 21);
});

await test("strict KEX refuses the chatter that is normally skipped", async () => {
    const s = fakeStream(plain(b(2) + "inserted") + plain(b(21)), 4);
    const t = transport.new(s, stubCrypto());
    const err = await assertRaises(() => t.expect(21, "NEWKEYS", true));
    assertEq(has(err, "strict KEX"), true, err);
});

await test("without strict KEX the same chatter is still tolerated", async () => {
    const s = fakeStream(plain(b(2) + "chatter") + plain(b(21)), 4);
    const t = transport.new(s, stubCrypto());
    assertEq((await t.expect(21, "NEWKEYS", false)).charCodeAt(0), 21);
});

await test("strict KEX refuses a global request rather than answering it", async () => {
    const gr = wire.writer().byte(80).string("x").boolean(true).build();
    const s = fakeStream(plain(gr) + plain(b(21)), 4);
    const t = transport.new(s, stubCrypto());
    await assertRaises(() => t.expect(21, "NEWKEYS", true),
                       "a global request during KEX should be refused");
});

// rekeying (RFC 4253 section 9) -----------------------------------------------

await test("a KEXINIT after the handshake is absorbed, not handed to the caller", async () => {
    const s = fakeStream(plain(b(20) + "rekey") + plain(b(94) + "data"), 4);
    const t = transport.new(s, stubCrypto());
    t.sessionId = "already-handshaken";
    let seen;
    t.runKex = async (_opts, iS) => { seen = iS; return [true]; };

    const m = await t.nextMessage();
    assertEq(seen !== undefined && seen.charCodeAt(0), 20, "the KEXINIT reaches run_kex:");
    assertEq(m.charCodeAt(0), 94, "and the caller gets the next real message:");
});

await test("a KEXINIT before the handshake is left for the exchange to read", async () => {
    const s = fakeStream(plain(b(20) + "first"), 4);
    const t = transport.new(s, stubCrypto());
    assertEq((await t.nextMessage()).charCodeAt(0), 20);
});

await test("a rekey is not re-entered while one is running", async () => {
    const s = fakeStream(plain(b(20) + "a") + plain(b(20) + "b"), 4);
    const t = transport.new(s, stubCrypto());
    t.sessionId = "sid";
    let calls = 0;
    t.runKex = async () => {
        calls = calls + 1;
        const inner = await t.nextMessage();
        assertEq(inner.charCodeAt(0), 20, "the inner read is NOT absorbed:");
        return [true];
    };
    await assertRaises(() => t.nextMessage());   // stream runs out after
    assertEq(calls, 1, "exactly one exchange:");
});

await test("a failed rekey stops the connection rather than carrying on", async () => {
    const s = fakeStream(plain(b(20) + "rekey"), 4);
    const t = transport.new(s, stubCrypto());
    t.sessionId = "sid";
    t.runKex = async () => [null, { code: "host_changed_midsession" }];
    const err = await assertRaises(() => t.nextMessage());
    assertEq(has(err, "host_changed_midsession"), true, err);
});

// rekeying from THIS side ------------------------------------------------

function fakeCipher(due, bytes, packets) {
    return {
        rekeyDue: () => due,
        bytesProcessed: () => bytes || 0,
        packetsSent: () => packets || 0,
    };
}

await test("a connection under its limits does not rekey", async () => {
    const t = transport.new(fakeStream("", 4), stubCrypto());
    t.sessionId = "sid";
    t.c2s = fakeCipher(false); t.s2c = fakeCipher(false);
    let ran = false;
    t.rekey = async () => { ran = true; return [true]; };
    assertEq(await t.maybeRekey(), false, "no exchange:");
    assertEq(ran, false, "and rekey was not called:");
});

await test("either direction reaching its limit asks for new keys", async () => {
    for (const which of ["c2s", "s2c"]) {
        const t = transport.new(fakeStream("", 4), stubCrypto());
        t.sessionId = "sid";
        t.c2s = fakeCipher(false); t.s2c = fakeCipher(false);
        t[which] = fakeCipher(true);
        let ran = false;
        t.rekey = async () => { ran = true; return [true]; };
        assertEq(await t.maybeRekey(), true, which + " exchanged:");
        assertEq(ran, true, which + " called rekey:");
    }
});

await test("a rekey that fails stops the connection rather than continuing", async () => {
    const t = transport.new(fakeStream("", 4), stubCrypto());
    t.sessionId = "sid";
    t.c2s = fakeCipher(true); t.s2c = fakeCipher(false);
    t.rekey = async () => [null, { code: "no_common_algorithm" }];
    const err = await assertRaises(() => t.maybeRekey());
    assertEq(has(err, "no_common_algorithm"), true, err);
});

await test("opening a session is where the limit is checked", async () => {
    const t = transport.new(fakeStream("", 4), stubCrypto());
    let asked = false;
    t.maybeRekey = async () => { asked = true; return false; };
    try { await t.openSession(); } catch (e) { /* the stream ends; that is fine */ }
    assertEq(asked, true, "open_session asked:");
});

await test("rekey before the handshake is refused, not attempted", async () => {
    const t = transport.new(fakeStream("", 4), stubCrypto());
    const [ok, why] = await t.rekey();
    assertEq(ok, null);
    assertEq(why.code, "not_handshaken");
});

await test("a rekey already running is not started a second time", async () => {
    const t = transport.new(fakeStream("", 4), stubCrypto());
    t.sessionId = "sid";
    t.inKex = true;
    t.runKex = async () => { throw new Error("run_kex must not be reached"); };
    assertEq((await t.rekey())[0], true);
});

await test("messages that arrive before the peer's KEXINIT are kept, in order", async () => {
    const t = transport.new(fakeStream("", 4), stubCrypto());
    t.deferMessage("first");
    t.deferMessage("second");
    assertEq(await t.nextMessage(), "first");
    assertEq(await t.nextMessage(), "second");
    // and then it reads from the stream again
    const u = transport.new(fakeStream(plain(b(94) + "live"), 4), stubCrypto());
    u.deferMessage(b(93) + "held");
    assertEq((await u.nextMessage()).charCodeAt(0), 93, "held first:");
    assertEq((await u.nextMessage()).charCodeAt(0), 94, "then the wire:");
});

await test("a rekey we start defers channel data until the exchange is over", async () => {
    const channelData = b(94) + "output";
    const theirs = kexinit.build({ kex: ["nope"], hostKey: ["nope"],
                                   cipher: ["nope"], mac: ["none"],
                                   compression: ["none"] }, "c".repeat(16));
    const s = fakeStream(plain(channelData) + plain(theirs), 4);
    const t = transport.new(s, stubCrypto());
    t.sessionId = "sid";

    const [ok, why] = await t.rekey();
    assertEq(ok, null, "the exchange failed as arranged:");
    assertEq(why.code, "no_common_algorithm");

    const held = await t.nextMessage();
    assertEq(held.charCodeAt(0), 94, "the channel data survived the rekey:");
    assertEq(held.slice(1), "output");
});

await test("a rekey we start reads its reply past the data it set aside", async () => {
    const channelData = b(94) + "output";
    const theirs = kexinit.build(null, "c".repeat(16));
    const reply = wire.writer().byte(31).string("K_S").string("q".repeat(32)).string("sig").build();
    const s = fakeStream(plain(channelData) + plain(theirs) + plain(reply), 4);
    const crypto = stubCrypto();
    crypto.x25519Keypair = () => [zeros(32), "\x11".repeat(32)];
    crypto.x25519 = () => [null, "stub stops here"];
    const t = transport.new(s, crypto);
    t.sessionId = "sid";

    let detail;
    try {
        const [res, why] = await t.rekey();
        detail = res ? "succeeded" : (why && why.code);
    } catch (e) {
        detail = errText(e);
    }
    assertEq(detail, "bad_kex_point", "the exchange read its own reply:");

    const held = await t.nextMessage();
    assertEq(held.charCodeAt(0), 94, "the channel data is still there for the caller:");
});

// strict KEX -------------------------------------------------------------------

function serverKexinit(strict) {
    const offer = offerWith({ kex: ["curve25519-sha256"] });
    if (strict) offer.kex = ["curve25519-sha256", kexinit.STRICT_S];
    return plain(kexinit.build(offer, "s".repeat(16)));
}

function kexCrypto() {
    const c = stubCrypto();
    c.x25519Keypair = () => [zeros(32), "\x11".repeat(32)];
    c.x25519 = () => [null, "stub stops here"];
    return c;
}

function ecdhReply() {
    return plain(wire.writer().byte(31).string("K_S").string("q".repeat(32)).string("sig").build());
}

const IGNORE = plain(b(2) + "x");

// Run the handshake; return its reason code, or the error it raised.
async function handshakeOutcome(inbound) {
    const t = transport.new(fakeStream("SSH-2.0-test\r\n" + inbound, 16), kexCrypto());
    try {
        const [res, why] = await t.handshake({ host: "h", trust: {} });
        return res ? "succeeded" : (why && why.code);
    } catch (e) {
        return errText(e);
    }
}

await test("strict KEX refuses a packet before the server's KEXINIT", async () => {
    const out = await handshakeOutcome(IGNORE + serverKexinit(true) + ecdhReply());
    assertEq(has(out, "was not its first packet"), true, out);
});

await test("without strict KEX a packet before KEXINIT is still tolerated", async () => {
    const out = await handshakeOutcome(IGNORE + serverKexinit(false) + ecdhReply());
    assertEq(out, "bad_kex_point", "reached the exchange:");
});

await test("strict KEX, decided by the first exchange, still bounds a rekey", async () => {
    const t = transport.new(fakeStream(serverKexinit(false)
                                       + plain(b(94) + "x")
                                       + ecdhReply(), 16),
                            kexCrypto());
    t.sessionId = "sid";
    t.strictKex = true;              // as the first exchange left it
    let ok = true, err;
    try { await t.nextMessage(); } catch (e) { ok = false; err = e; }
    assertEq(ok, false);
    assertEq(has(errText(err), "strict KEX"), true, errText(err));
});

await test("sequence numbers count every packet, both ways", async () => {
    const t = transport.new(fakeStream(IGNORE + plain(b(20) + "x"), 16), stubCrypto());
    await t.nextMessage();
    assertEq(t.recvSeq, 2, "the skipped IGNORE counts too:");
    await t.sendPacket(b(2));
    assertEq(t.sendSeq, 1);
});

await test("stats span the connection, not just the current key", async () => {
    const t = transport.new(fakeStream("", 4), stubCrypto());
    t.rekeys = 2;
    t.totalSent = 100; t.totalReceived = 200;
    t.c2s = fakeCipher(false, 7, 3);
    t.s2c = fakeCipher(false, 9, 4);
    const st = t.stats();
    assertEq(st.rekeys, 2);
    assertEq(st.bytesSent, 107, "carried across rekeys:");
    assertEq(st.bytesReceived, 209);
    assertEq(st.packetsSent, 3, "packets are per-key:");
    assertEq(st.packetsReceived, 4);
    assertEq(st.rekeyDue, false);
});

// liveness ------------------------------------------------------------------
//
// A stream that follows a script, one step per read: a string is data, and
// "TIMEOUT" / "DEADLINE" are the stream's bounded-wait expiries.

function scriptedStream(steps) {
    const s = fakeStream("", 1);
    s.steps = steps; s.i = 0; s.deadlines = []; s.waits = [];
    s.read = function (n) {
        const idx = this.i;
        this.i = this.i + 1;
        let step = this.steps[idx];
        if (step === undefined) return [""];
        if (step === "TIMEOUT") return [null, "timed out", "timeout"];
        if (step === "DEADLINE") return [null, "deadline reached", "deadline"];
        if (step.length > n) {
            this.steps.splice(idx + 1, 0, step.slice(n));
            step = step.slice(0, n);
        }
        return [step];
    };
    s.deadline = function (ms) { this.deadlines.push(ms); };
    s.wait = function (ms) { this.waits.push(ms); };
    return s;
}

function keepalivesSent(s) {
    let n = 0;
    for (const p of s.written) {
        const [payload] = packet.parse(p, 8);
        if (payload && payload.charCodeAt(0) === 80) n++;
    }
    return n;
}

async function raisedCode(fn) {
    try {
        await fn();
    } catch (e) {
        return (e && typeof e === "object" && e.code) ? e.code : errText(e);
    }
    return "no error";
}

await test("the stream is given the keepalive interval as its wait bound", async () => {
    const s = scriptedStream([]);
    transport.new(s, stubCrypto(), { keepaliveMs: 30000 });
    assertEq(s.waits[0], 30000);
});

await test("a silent server is given up on at the idle bound, keepalive in between", async () => {
    const s = scriptedStream(["TIMEOUT", "TIMEOUT"]);
    const t = transport.new(s, stubCrypto(), { keepaliveMs: 30000, idleMs: 60000 });
    t.authenticated = true;
    assertEq(await raisedCode(() => t.fill(1)), "timeout");
    assertEq(keepalivesSent(s), 1, "one keepalive before giving up:");
    const [ch, err] = await t.openSession();
    assertEq(ch, null);
    assertEq(err.code, "timeout");
});

await test("a server that answers keepalives is not idle, however quiet", async () => {
    const reply = plain(b(82));
    const steps = [];
    for (let i = 0; i < 10; i++) { steps.push("TIMEOUT"); steps.push(reply); }
    steps.push(plain(b(20) + "real"));
    const s = scriptedStream(steps);
    const t = transport.new(s, stubCrypto(), { keepaliveMs: 30000, idleMs: 60000 });
    t.authenticated = true;
    assertEq((await t.nextMessage()).charCodeAt(0), 20, "the reply is absorbed:");
    assertEq(keepalivesSent(s), 10);
});

await test("no keepalive before authentication, and the idle bound still holds", async () => {
    const s = scriptedStream(["TIMEOUT", "TIMEOUT"]);
    const t = transport.new(s, stubCrypto(), { keepaliveMs: 30000, idleMs: 60000 });
    assertEq(await raisedCode(() => t.fill(1)), "timeout");
    assertEq(keepalivesSent(s), 0);
});

await test("with no idle bound, unanswered keepalives end it", async () => {
    const s = scriptedStream(["TIMEOUT", "TIMEOUT", "TIMEOUT", "TIMEOUT"]);
    const t = transport.new(s, stubCrypto(), { keepaliveMs: 30000, keepaliveMax: 3, idleMs: 0 });
    t.authenticated = true;
    assertEq(await raisedCode(() => t.fill(1)), "timeout");
    assertEq(keepalivesSent(s), 3);
});

await test("a deadline is its own code, and does not kill the connection", async () => {
    const s = scriptedStream(["DEADLINE"]);
    const t = transport.new(s, stubCrypto());
    assertEq(await raisedCode(() => t.fill(1)), "deadline");
    assertEq(t.dead, undefined);
});

await test("a write that waited too long is retried, not failed", async () => {
    const s = scriptedStream([]);
    let tries = 0;
    s.write = function (bytes) {
        tries = tries + 1;
        if (tries === 1) return [null, "timed out", "timeout"];
        this.written.push(bytes);
        return [true];
    };
    const t = transport.new(s, stubCrypto(), { idleMs: 60000 });
    await t.sendRaw("abc");
    assertEq(tries, 2);
    assertEq(s.written[0], "abc");
});

await test("exec with timeout_ms returns a timeout and keeps the connection", async () => {
    const s = scriptedStream([conf(), okReply(), data("partial"), "DEADLINE", eof(), fin()]);
    const t = transport.new(s, stubCrypto());
    const [r, err] = await t.exec("sleep 600", { timeoutMs: 1500 });
    assertEq(r, null);
    assertEq(err.code, "timeout");
    assertEq(t.dead, undefined, "the connection is still usable:");
    assertEq(hasType(s, 97), true, "the channel was closed:");
    assertEq(s.deadlines.join(","), "1500,0,5000,0");
});

await test("exec refuses a timeout_ms that is not a positive integer", async () => {
    const t = transport.new(scriptedStream([]), stubCrypto());
    for (const bad of [0, -1, 1.5, "10"]) {
        const [r, err] = await t.exec("x", { timeoutMs: bad });
        assertEq(r, null);
        assertEq(err.code, "bad_timeout", String(bad));
    }
});

// sftp ----------------------------------------------------------------------

// One SFTP message from the server, inside channel data - split across
// messages the way a real server splits one larger than our packet size.
function sreply(payload) {
    const bytes = sftpCodec.frame(payload), out = [];
    for (let i = 0; i < bytes.length; i += 16384) out.push(data(bytes.slice(i, i + 16384)));
    return out.join("");
}
const sVersion = () => sreply(wire.writer().byte(2).uint32(3).build());
const sHandle = (id, h) => sreply(wire.writer().byte(102).uint32(id).string(h).build());
const sData = (id, d) => sreply(wire.writer().byte(103).uint32(id).string(d).build());
const sStatus = (id, code) => sreply(wire.writer().byte(101).uint32(id).uint32(code)
    .string("").string("en").build());

// The SFTP requests we sent, decoded: { type, id, handle? } in order.
function sentSftp(s) {
    const body0 = [];
    for (const p of s.written) {
        const [payload] = packet.parse(p, 8);
        if (payload && payload.charCodeAt(0) === 94) {
            const r = wire.reader(payload);
            r.byte(); r.uint32();
            body0.push(r.string());
        }
    }
    const body = body0.join("");
    const out = [];
    let pos = 0;
    while (pos + 5 <= body.length) {
        const n = wire.peekUint32(body, pos);
        const req = body.slice(pos + 4, pos + 4 + n);
        pos = pos + 4 + n;
        const rr = wire.reader(req);
        const ty = rr.byte();
        const e = { type: ty };
        if (ty !== 1) {                            // INIT carries no id
            e.id = rr.uint32();
            if (ty === 4 || ty === 8) {            // CLOSE, FSTAT
                e.handle = rr.string();
            } else if (ty === 5) {                 // READ
                e.handle = rr.string(); e.offset = rr.uint64(); e.len = rr.uint32();
            } else if (ty === 6) {                 // WRITE
                e.handle = rr.string(); e.offset = rr.uint64(); e.data = rr.string();
            } else if (ty === 3) {                 // OPEN
                e.path = rr.string(); e.pflags = rr.uint32();
                e.attrs = sftpCodec.decodeAttrs(rr);
            } else if (ty === 14 || ty === 9) {    // MKDIR, SETSTAT
                e.path = rr.string(); e.attrs = sftpCodec.decodeAttrs(rr);
            }
        }
        out.push(e);
    }
    return out;
}

function sentOfType(s, ty) {
    return sentSftp(s).filter((e) => e.type === ty);
}

function sAttrs(id, a) {
    return sreply(wire.writer().byte(105).uint32(id).raw(sftpCodec.encodeAttrs(a)).build());
}

// A session to script: conf, subsystem reply and VERSION, then `rest`.
async function sftpOver(rest, chunk, window) {
    const s = fakeStream(conf(window) + okReply() + sVersion() + rest, chunk || 11);
    const t = transport.new(s, stubCrypto());
    const [f, err] = await t.sftp();
    assertOk(f, "sftp: " + (err && err.code));
    return [f, s];
}

await test("a request cut short mid-frame leaves the session broken (#1009 audit)", async () => {
    // A 20-byte window: INIT takes 9, so the STAT frame goes out in part and
    // the wait for more window then meets the end of the stream.
    const [f, s] = await sftpOver("", 11, 20);
    await assertRaises(() => f.stat("/a/rather/long/path/name"), "the stat");
    const before = s.written.length;
    const msg = await assertRaises(() => f.stat("/x"), "a later request");
    assertOk(has(msg, "cut short"), "says why: " + msg);
    assertEq(s.written.length, before, "and sends nothing:");
});

await test("stat returns the attributes the server sent", async () => {
    const [f] = await sftpOver(sAttrs(1, { size: 1234, permissions: 0x41ed,
                                           uid: 1, gid: 2, atime: 5, mtime: 6 }));
    const [a] = await f.stat("/srv");
    assertEq(a.size, 1234);
    assertEq(a.permissions, 0x41ed);
    assertEq(a.isDir, true);
    assertEq(a.mtime, 6);
});

await test("a status by name: a missing file is no_such_file", async () => {
    const [f] = await sftpOver(sStatus(1, 2));
    const [a, err] = await f.stat("/nope");
    assertEq(a, null);
    assertEq(err.code, "no_such_file");
});

await test("mkdir sends an octal-string mode as its permission bits", async () => {
    const [f, s] = await sftpOver(sStatus(1, 0));
    assertEq((await f.mkdir("/srv/app", "755"))[0], true);
    assertEq(sentOfType(s, 14)[0].attrs.permissions, 493);
});

await test("rename, remove, rmdir and chmod report success and failure", async () => {
    const [f, s] = await sftpOver(sStatus(1, 0) + sStatus(2, 0) + sStatus(3, 4) + sStatus(4, 0));
    assertEq((await f.rename("/a", "/b"))[0], true);
    assertEq((await f.remove("/b"))[0], true);
    const [ok, err] = await f.rmdir("/full");
    assertEq(ok, null);
    assertEq(err.code, "failure");
    assertEq((await f.chmod("/x", "0644"))[0], true);
    assertEq(sentOfType(s, 9)[0].attrs.permissions, 420);
});

await test("a mode that is not octal is a caller error, with a code", async () => {
    const [f] = await sftpOver("");
    let ok = true, err;
    try { await f.chmod("/x", "rwx"); } catch (e) { ok = false; err = e; }
    assertEq(ok, false);
    assertEq(err.code, "bad_argument");
});

await test("open modes map to their flags", async () => {
    const cases = { "r": 0x01, "r+": 0x03, "w": 0x1a, "a": 0x0e, "wx": 0x2a };
    for (const mode of Object.keys(cases)) {
        const [f, s] = await sftpOver(sHandle(1, "h"));
        assertOk((await f.open("/f", mode))[0]);
        assertEq(sentOfType(s, 3)[0].pflags, cases[mode], mode);
    }
});

await test("open with opts.mode creates the file with those permissions", async () => {
    const [f, s] = await sftpOver(sHandle(1, "h"));
    assertOk((await f.open("/f", "w", { mode: "600" }))[0]);
    assertEq(sentOfType(s, 3)[0].attrs.permissions, 384);
});

await test("a large read ramps up to several requests in flight", async () => {
    const C = 32768;
    const full = "a".repeat(C);
    const [f] = await sftpOver(sHandle(1, "h")
        + sData(2, full)
        + sData(4, full) + sData(3, full)
        + sData(5, "tail") + sStatus(6, 1) + sStatus(7, 1) + sStatus(8, 1));
    const [file] = await f.open("/big", "r");
    assertOk(file);
    const [got] = await file.read(10 * C);
    assertEq(got.length, 3 * C + 4, "three full chunks and the tail:");
    assertEq(got.slice(-4), "tail");
    assertEq(file.tell(), 3 * C + 4);
});

await test("a read keeps the window full instead of draining it between batches", async () => {
    const C = 32768, N = 200;
    const full = "r".repeat(C);
    const script = [sHandle(1, "h")];
    const ends = [];
    const head = conf() + okReply() + sVersion() + sHandle(1, "h");
    let at = head.length;
    for (let i = 1; i <= N; i++) {
        const rpl = sData(i + 1, full);
        script.push(rpl);
        at = at + rpl.length;
        ends.push(at);
    }
    const s = fakeStream(conf() + okReply() + sVersion() + script.join(""), 4096);
    let reads = 0;
    const inflight = [];
    const write = s.write;
    s.write = function (bytes) {
        const [p] = packet.parse(bytes, 8);
        if (p && p.charCodeAt(0) === 94) {
            const r = wire.reader(p); r.byte(); r.uint32();
            const body = r.string();
            if (body.length > 5 && body.charCodeAt(4) === 5) {
                reads = reads + 1;
                let taken = 0;
                // Lua's `e < self._pos` with a 1-based _pos is e <= _pos here.
                for (const e of ends) if (e <= this._pos) taken++;
                inflight.push(reads - taken);
            }
        }
        return write.call(this, bytes);
    };
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);
    const [file] = await f.open("/big", "r");
    assertOk(file);
    assertEq((await file.read(N * C))[0].length, N * C);
    assertEq(reads, N);
    let peak = 0, atPeak = null;
    inflight.forEach((v, i) => { if (v > peak) { peak = v; atPeak = i; } });
    assertEq(peak, 64, "the window reaches 64 requests:");
    for (let i = atPeak; i < inflight.length; i++) {
        assertEq(inflight[i] >= 63, true, "request " + (i + 1) + " sent with "
                 + inflight[i] + " in flight");
    }
});

await test("a READ reply longer than the request is refused", async () => {
    const [f] = await sftpOver(sHandle(1, "h") + sData(2, "toolong"));
    const [file] = await f.open("/f", "r");
    assertOk(file);
    const [d, err] = await file.read(3);
    assertEq(d, null);
    assertEq(err.code, "bad_reply");
});

await test("a failed write stops sending and still collects what is in flight", async () => {
    const C = 32768;
    const [f, s] = await sftpOver(sHandle(1, "h") + sStatus(2, 0) + sStatus(3, 4)
        + sStatus(4, 0) + sStatus(5, 0) + sStatus(6, 0), 4096, 1048576);
    const [file] = await f.open("/f", "w");
    assertOk(file);
    const [ok, err] = await file.write("w".repeat(4 * C));
    assertEq(ok, null);
    assertEq(err.code, "failure");
    assertEq((await file.close())[0], true);
    assertEq(sentOfType(s, 6).length, 4);
});

await test("replies are matched by id even when the server reorders them", async () => {
    const C = 32768;
    const a = "1".repeat(C), bb = "2".repeat(C);
    const [f] = await sftpOver(sHandle(1, "h") + sData(2, a) + sData(4, "end") + sData(3, bb));
    const [file] = await f.open("/f", "r");
    assertOk(file);
    const [got] = await file.read(3 * C);
    assertEq(got, a + bb + "end", "in offset order, not arrival order:");
});

await test("a large write goes out as pipelined chunks at the right offsets", async () => {
    const C = 32768;
    const payload = "w".repeat(3 * C + 10);
    const [f, s] = await sftpOver(sHandle(1, "h") + sStatus(2, 0) + sStatus(3, 0)
        + sStatus(4, 0) + sStatus(5, 0) + sStatus(6, 0), 4096, 1048576);
    const [file] = await f.open("/f", "w");
    assertOk(file);
    assertEq((await file.write(payload))[0], true);
    assertEq((await file.close())[0], true);
    const writes = sentOfType(s, 6);
    assertEq(writes.length, 4);
    writes.forEach((w, i) => assertEq(w.offset, i * C, "chunk " + (i + 1)));
    assertEq(writes[3].data.length, 10);
});

await test("seek moves where the next read starts; a closed file says so", async () => {
    const [f, s] = await sftpOver(sHandle(1, "h") + sData(2, "xyz") + sStatus(3, 0));
    const [file] = await f.open("/f", "r");
    assertOk(file);
    file.seek(100);
    assertEq((await file.read(3))[0], "xyz");
    assertEq(sentOfType(s, 5)[0].offset, 100);
    assertEq((await file.close())[0], true);
    assertEq((await file.close())[0], true, "closing twice is harmless:");
    const [d, err] = await file.read(1);
    assertEq(d, null);
    assertEq(err.code, "closed");
});

await test("a failed sftp read does not shift the next request's replies", async () => {
    const s = fakeStream(conf() + okReply() + sVersion()
        + sHandle(1, "hA") + sData(2, "xxxxx") + sStatus(3, 0)
        + sHandle(4, "hB") + sStatus(5, 0) + sStatus(6, 0), 11);
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);

    const [d, err] = await f.read("/a", 4);
    assertEq(d, null);
    assertEq(err.code, "too_large");
    assertEq(has(err.detail, "byte limit"), true, err.detail);

    assertEq((await f.write("/b", "hello"))[0], true, "the write should succeed:");

    const writes = [];
    for (const e of sentSftp(s)) if (e.type === 6) writes.push(e.handle);
    assertEq(writes.length, 1);
    assertEq(writes[0], "hB", "the write must go to the file it opened:");
});

await test("an sftp reply for another request is refused", async () => {
    const s = fakeStream(conf() + okReply() + sVersion() + sStatus(7, 0));
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);
    const err = await assertRaises(() => f.realpath("/x"));
    assertEq(has(err, "reply for request 7 while waiting for 1"), true, err);
});

await test("closing sftp drains its channel, so the next reader is not handed its tail", async () => {
    const on = (w) => plain(w.build());
    const ch1Conf = on(wire.writer().byte(91).uint32(1).uint32(8).uint32(65536).uint32(32768));
    const ch1Ok = on(wire.writer().byte(99).uint32(1));
    const ch1Data = on(wire.writer().byte(94).uint32(1).string("hi"));
    const ch1St = on(wire.writer().byte(98).uint32(1).string("exit-status")
        .boolean(false).uint32(0));
    const ch1Eof = on(wire.writer().byte(96).uint32(1));
    const ch1Fin = on(wire.writer().byte(97).uint32(1));

    const s = fakeStream(conf() + okReply() + sVersion()
        + eof() + fin()                                   // sftp channel's tail
        + ch1Conf + ch1Ok + ch1Data + ch1St + ch1Eof + ch1Fin, 9);
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);
    await f.close();
    assertEq(f.ch.closed, true, "the peer's CLOSE was read:");
    const [r] = await t.exec("echo hi");
    assertEq(r.stdout, "hi");
    assertEq(r.status, 0);
});

// Two channels on one connection.

const on = (w) => plain(w.build());
function execOnCh1(extraMid) {
    return on(wire.writer().byte(91).uint32(1).uint32(8).uint32(65536).uint32(32768))
        + on(wire.writer().byte(99).uint32(1))
        + extraMid
        + on(wire.writer().byte(94).uint32(1).string("hi"))
        + on(wire.writer().byte(98).uint32(1).string("exit-status").boolean(false).uint32(0))
        + on(wire.writer().byte(96).uint32(1))
        + on(wire.writer().byte(97).uint32(1));
}

await test("a window adjust for an open sftp channel does not break an exec", async () => {
    const adj = on(wire.writer().byte(93).uint32(0).uint32(4096));
    const s = fakeStream(conf() + okReply() + sVersion() + execOnCh1(adj), 9);
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);
    const before = f.ch.sendWindow;
    const [r] = await t.exec("echo hi");
    assertOk(r);
    assertEq(r.stdout, "hi");
    assertEq(f.ch.sendWindow, before + 4096, "the adjust reached the sftp channel:");
});

await test("sftp data that arrives during an exec is kept for the sftp session", async () => {
    const s = fakeStream(conf() + okReply() + sVersion() + execOnCh1(sStatus(1, 0)), 9);
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);
    const [r] = await t.exec("echo hi");
    assertOk(r);
    assertEq(r.stdout, "hi");
    assertEq((await f.remove("/x"))[0], true, "the queued reply answers the request:");
});

await test("sftp data that arrives while an exec channel opens is not dropped", async () => {
    const mid = sStatus(1, 0);
    const s = fakeStream(conf() + okReply() + sVersion() + mid + execOnCh1(""), 9);
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);
    assertOk((await t.exec("echo hi"))[0]);
    assertEq((await f.remove("/x"))[0], true);
});

await test("a keepalive on the sftp channel during an exec is answered at once", async () => {
    const ka = on(wire.writer().byte(98).uint32(0).string("keepalive@openssh.com").boolean(true));
    const s = fakeStream(conf() + okReply() + sVersion() + execOnCh1(ka), 9);
    const t = transport.new(s, stubCrypto());
    assertOk((await t.sftp())[0]);
    assertOk((await t.exec("echo hi"))[0]);
    let answered = false;
    for (const pkt of s.written) {
        const [payload] = packet.parse(pkt, 8);
        if (payload && payload.charCodeAt(0) === 100 && wire.peekUint32(payload, 1) === 7) {
            answered = true;
        }
    }
    assertEq(answered, true, "CHANNEL_FAILURE to the sftp channel (server id 7):");
});

// An sftp session that stops being answered.
function stallingStream(inbound) {
    const s = fakeStream(inbound, 9);
    s.deadlines = [];
    s.armed = 0;
    s.deadline = function (ms) {
        this.deadlines.push(ms);
        this.armed = ms;
    };
    const read = s.read;
    s.read = function (n) {
        if (this._pos >= this._in.length && this.armed > 0) {
            return [null, "deadline reached", "deadline"];
        }
        return read.call(this, n);
    };
    return s;
}

await test("an sftp request nobody answers times out and closes the session", async () => {
    const s = stallingStream(conf() + okReply() + sVersion());
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp({ replyTimeoutMs: 1000 });
    assertOk(f);
    let ok = true, err;
    try { await f.stat("/x"); } catch (e) { ok = false; err = e; }
    assertEq(ok, false);
    assertEq(err && typeof err === "object" && err.code, "timeout");
    let ok2 = true, err2;
    try { await f.stat("/y"); } catch (e) { ok2 = false; err2 = e; }
    assertEq(ok2, false);
    assertEq(err2.code, "timeout");
    let closed = false;
    for (const pkt of s.written) {
        const [payload] = packet.parse(pkt, 8);
        if (payload && payload.charCodeAt(0) === 97 && wire.peekUint32(payload, 1) === 7) {
            closed = true;
        }
    }
    assertEq(closed, true, "CHANNEL_CLOSE sent:");
    assertEq(t.dead && t.dead.code, "timeout");
});

await test("the sftp reply deadline is armed per message and cleared after", async () => {
    const s = stallingStream(conf() + okReply() + sVersion() + sStatus(1, 0));
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp({ replyTimeoutMs: 1000 });
    assertOk(f);
    assertEq((await f.remove("/x"))[0], true);
    assertEq(s.armed, 0, "no deadline left armed:");
    let armed = 0;
    for (const ms of s.deadlines) if (ms === 1000) armed++;
    assertEq(armed >= 2, true, "armed for the version and for the reply:");
});

await test("reply_timeout_ms is checked", async () => {
    for (const bad of [-1, 1.5, "60", 86400001]) {
        const s = fakeStream(conf() + okReply() + sVersion(), 9);
        const t = transport.new(s, stubCrypto());
        let ok = true, err;
        try { await t.sftp({ replyTimeoutMs: bad }); } catch (e) { ok = false; err = e; }
        assertEq(ok, false, String(bad));
        assertEq(err.code, "bad_argument");
    }
});

await test("closing sftp twice sends one CLOSE", async () => {
    const s = fakeStream(conf() + okReply() + sVersion() + fin(), 9);
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);
    await f.close();
    await f.close();
    let closes = 0;
    for (const ty of writtenTypes(s)) if (ty === 97) closes++;
    assertEq(closes, 1);
});

// The CHANNEL_DATA sizes we sent, in order.
function sentDataSizes(s) {
    const out = [];
    for (const p of s.written) {
        const [payload] = packet.parse(p, 8);
        if (payload && payload.charCodeAt(0) === 94) {
            const r = wire.reader(payload);
            r.byte(); r.uint32();
            out.push(r.string().length);
        }
    }
    return out;
}

await test("an sftp write fits the peer's packet size", async () => {
    const s = fakeStream(conf(1048576, 8192) + okReply() + sVersion()
        + sHandle(1, "h") + sStatus(2, 0) + sStatus(3, 0), 11);
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);
    assertEq((await f.write("/big", "z".repeat(16384)))[0], true);
    for (const n of sentDataSizes(s)) {
        assertEq(n <= 8192, true, "sent " + n + " bytes in one message");
    }
});

await test("an sftp write waits for the window instead of raising", async () => {
    const s = fakeStream(conf(2000, 32768) + okReply() + sVersion()
        + sHandle(1, "h") + grant(1048576) + sStatus(2, 0) + sStatus(3, 0), 11);
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);
    assertEq((await f.write("/w", "z".repeat(5000)))[0], true);
});

await test("exec stdin is chunked below our own limit, whatever the peer allows", async () => {
    const s = fakeStream(conf(1048576, 262144) + okReply() + status(0) + eof() + fin(), 7);
    const t = transport.new(s, stubCrypto());
    const [r] = await t.exec("cat", { stdin: "y".repeat(100 * 1024) });
    assertEq(r.status, 0);
    let total = 0;
    for (const n of sentDataSizes(s)) {
        assertEq(n <= 32768, true, "sent " + n + " bytes in one message");
        total = total + n;
    }
    assertEq(total, 100 * 1024);
});

await test("an sftp write closes its handle and reads the answer on failure", async () => {
    const s = fakeStream(conf() + okReply() + sVersion()
        + sHandle(1, "hW") + sStatus(2, 4) + sStatus(3, 0)
        + sHandle(4, "hR") + sData(5, "ok") + sStatus(6, 1)
        + sStatus(7, 0), 5);
    const t = transport.new(s, stubCrypto());
    const [f] = await t.sftp();
    assertOk(f);
    const [ok, err] = await f.write("/w", "data");
    assertEq(ok, null);
    assertEq(err.code, "failure");
    assertEq(err.status, 4);
    assertEq((await f.read("/r"))[0], "ok");
});

// userauth --------------------------------------------------------------------

const acceptService = () => plain(wire.writer().byte(6).string("ssh-userauth").build());
const authFailure = (methods, partial) =>
    plain(wire.writer().byte(51).namelist(methods).boolean(partial || false).build());
const authSuccess = () => plain("\x34");

// The public key algorithm named in each USERAUTH_REQUEST written.
function requestedAlgorithms(s) {
    const out = [];
    for (const w of s.written) {
        const payload = w.slice(0, 4) !== "SSH-" && packet.parse(w, 8)[0];
        if (payload && payload.charCodeAt(0) === 50) {
            const r = wire.reader(payload);
            r.byte(); r.string(); r.string();
            if (r.string() === "publickey") {
                r.boolean();
                out.push(r.string());
            }
        }
    }
    return out.join(",");
}

function authCrypto(signed) {
    const c = stubCrypto();
    c.sign = (alg) => { signed.push(alg); return "\x07".repeat(256); };
    c.ed25519Sign = () => { signed.push("ed25519"); return "\x08".repeat(64); };
    return c;
}

const RSA_KEY = { algorithm: "ssh-rsa", pem: "PEM",
                  blob: wire.writer().string("ssh-rsa").mpint("\x01\x00\x01").mpint("\xff").build() };
const ED_KEY = { algorithm: "ssh-ed25519", secret: "\x03".repeat(64),
                 blob: wire.writer().string("ssh-ed25519").string("\x04".repeat(32)).build() };

function authenticating(inbound, signed) {
    const s = fakeStream(inbound, 64);
    const t = transport.new(s, authCrypto(signed));
    t.sessionId = "SESSION";
    return [t, s];
}

await test("an RSA key refused under SHA-512 is offered again under SHA-256", async () => {
    const signed = [];
    const [t, s] = authenticating(acceptService() + authFailure(["publickey"]) + authSuccess(), signed);
    assertEq((await t.authenticate("u", RSA_KEY))[0], true);
    assertEq(signed.join(","), "RS512,RS256");
    assertEq(requestedAlgorithms(s), "rsa-sha2-512,rsa-sha2-256");
});

await test("an RSA key accepted under SHA-512 is signed once", async () => {
    const signed = [];
    const [t, s] = authenticating(acceptService() + authSuccess(), signed);
    assertEq((await t.authenticate("u", RSA_KEY))[0], true);
    assertEq(signed.join(","), "RS512");
    assertEq(requestedAlgorithms(s), "rsa-sha2-512");
});

await test("no second hash once the server stops offering publickey", async () => {
    const signed = [];
    const [t, s] = authenticating(acceptService() + authFailure(["password"]), signed);
    const [ok, err] = await t.authenticate("u", RSA_KEY);
    assertEq(ok, null);
    assertEq(err.code, "auth_failed");
    assertEq(requestedAlgorithms(s), "rsa-sha2-512");
});

await test("partial success is not retried under another hash", async () => {
    const signed = [];
    const [t, s] = authenticating(acceptService() + authFailure(["publickey", "password"], true),
                                  signed);
    const [ok, err] = await t.authenticate("u", RSA_KEY);
    assertEq(ok, null);
    assertEq(err.code, "partial_success");
    assertEq(requestedAlgorithms(s), "rsa-sha2-512");
});

await test("both RSA hashes refused is an ordinary auth failure", async () => {
    const signed = [];
    const [t] = authenticating(acceptService() + authFailure(["publickey"])
                               + authFailure(["publickey"]), signed);
    const [ok, err] = await t.authenticate("u", RSA_KEY);
    assertEq(ok, null);
    assertEq(err.code, "auth_failed");
    assertEq(signed.join(","), "RS512,RS256");
});

const EC_KEY = { algorithm: "ecdsa-sha2-nistp384", pem: "PEM",
                 blob: wire.writer().string("ecdsa-sha2-nistp384").string("nistp384")
                     .string("\x04" + "\x05".repeat(96)).build() };

await test("an ECDSA key signs under its own curve's hash, once", async () => {
    const signed = [];
    const c = authCrypto(signed);
    c.sign = (alg) => { signed.push(alg); return "\x09".repeat(96); };
    const s = fakeStream(acceptService() + authSuccess(), 64);
    const t = transport.new(s, c);
    t.sessionId = "SESSION";
    assertEq((await t.authenticate("u", EC_KEY))[0], true);
    assertEq(signed.join(","), "ES384");
    assertEq(requestedAlgorithms(s), "ecdsa-sha2-nistp384");
});

await test("a refused ECDSA key is not retried", async () => {
    const signed = [];
    const c = authCrypto(signed);
    c.sign = (alg) => { signed.push(alg); return "\x09".repeat(96); };
    const s = fakeStream(acceptService() + authFailure(["publickey"]), 64);
    const t = transport.new(s, c);
    t.sessionId = "SESSION";
    const [ok, err] = await t.authenticate("u", EC_KEY);
    assertEq(ok, null);
    assertEq(err.code, "auth_failed");
    assertEq(requestedAlgorithms(s), "ecdsa-sha2-nistp384");
});

await test("an Ed25519 key is offered once, as before", async () => {
    const signed = [];
    const [t, s] = authenticating(acceptService() + authFailure(["publickey"]), signed);
    const [ok, err] = await t.authenticate("u", ED_KEY);
    assertEq(ok, null);
    assertEq(err.code, "auth_failed");
    assertEq(signed.join(","), "ed25519");
    assertEq(requestedAlgorithms(s), "ssh-ed25519");
});

// Several keys: offered in order until one is accepted -------------------------

await test("a refused key gives way to the next, and the accepted one is recorded", async () => {
    const signed = [];
    const [t, s] = authenticating(acceptService() + authFailure(["publickey"]) + authSuccess(), signed);
    assertEq((await t.authenticate("u", [ED_KEY, EC_KEY]))[0], true);
    assertEq(requestedAlgorithms(s), "ssh-ed25519,ecdsa-sha2-nistp384");
    assertEq(t.stats().authKey, 1);          // 0-based (Lua: 2)
});

await test("an RSA key still tries both hashes before the next key", async () => {
    const signed = [];
    const [t, s] = authenticating(acceptService() + authFailure(["publickey"])
                                  + authFailure(["publickey"]) + authSuccess(), signed);
    assertEq((await t.authenticate("u", [RSA_KEY, ED_KEY]))[0], true);
    assertEq(requestedAlgorithms(s), "rsa-sha2-512,rsa-sha2-256,ssh-ed25519");
    assertEq(t.stats().authKey, 1);          // 0-based (Lua: 2)
});

await test("every key refused is one auth failure that says how many were tried", async () => {
    const signed = [];
    const [t, s] = authenticating(acceptService() + authFailure(["publickey"])
                                  + authFailure(["publickey"]), signed);
    const [ok, err] = await t.authenticate("u", [ED_KEY, EC_KEY]);
    assertEq(ok, null);
    assertEq(err.code, "auth_failed");
    assertEq(err.detail, "none of the 2 keys was accepted");
    assertEq(requestedAlgorithms(s), "ssh-ed25519,ecdsa-sha2-nistp384");
});

await test("partial success stops at the key that earned it", async () => {
    const signed = [];
    const [t, s] = authenticating(acceptService()
                                  + authFailure(["publickey", "password"], true), signed);
    const [ok, err] = await t.authenticate("u", [ED_KEY, EC_KEY]);
    assertEq(ok, null);
    assertEq(err.code, "partial_success");
    assertEq(requestedAlgorithms(s), "ssh-ed25519");
});

await test("no next key once the server stops offering publickey", async () => {
    const signed = [];
    const [t, s] = authenticating(acceptService() + authFailure(["password"]), signed);
    const [ok, err] = await t.authenticate("u", [ED_KEY, EC_KEY]);
    assertEq(ok, null);
    assertEq(err.code, "auth_failed");
    assertEq(requestedAlgorithms(s), "ssh-ed25519");
});

await test("a single key is still accepted as a key, not a list", async () => {
    const signed = [];
    const [t] = authenticating(acceptService() + authSuccess(), signed);
    assertEq((await t.authenticate("u", ED_KEY))[0], true);
    assertEq(t.stats().authKey, 0);          // 0-based (Lua: 1)
});

// audit 4 ---------------------------------------------------------------------

await test("strict KEX refuses a CHANNEL_OPEN injected into the exchange", async () => {
    const open = wire.writer().byte(90).string("x11").uint32(1).uint32(0).uint32(0).build();
    const s = fakeStream(plain(open) + plain(b(21)), 4);
    const t = transport.new(s, stubCrypto());
    const err = await assertRaises(() => t.expect(21, "NEWKEYS", true));
    assertEq(has(err, "strict KEX"), true, err);
    assertEq(s.written.length, 0, "nothing may be sent mid-exchange:");
});

await test("strict KEX refuses a REQUEST_SUCCESS injected into the exchange", async () => {
    const s = fakeStream(plain(b(81)) + plain(b(21)), 4);
    const t = transport.new(s, stubCrypto());
    const err = await assertRaises(() => t.expect(21, "NEWKEYS", true));
    assertEq(has(err, "strict KEX"), true, err);
});

await test("a send while another is in flight is refused before sealing", async () => {
    const s = fakeStream("", 4);
    const t = transport.new(s, stubCrypto());
    let nested;
    s.write = async function (bytes) {
        if (nested === undefined) {
            try { await t.sendPacket("\x02"); nested = null; } catch (e) { nested = e; }
        }
        this.written.push(bytes);
        return [true];
    };
    await t.sendPacket("\x02");
    assertEq(nested && typeof nested === "object" && nested.code, "busy", String(nested));
    assertEq(t.sendSeq, 1, "the refused send must not count:");
    assertEq(s.written.length, 1);
    assertEq(t.dead, undefined, "a refused second sender does not kill the connection:");
});

await test("a sealed packet that cannot be written marks the connection dead", async () => {
    const s = fakeStream("", 4);
    s.write = () => [null, "boom", "error"];
    const t = transport.new(s, stubCrypto());
    t.c2s = { seal: () => "sealed" };
    await assertRaises(() => t.sendPacket("\x02"));
    assertEq(t.dead && t.dead.code, "send_failed");
    let ok = true, err;
    try { await t.sendPacket("\x02"); } catch (e) { ok = false; err = e; }
    assertEq(ok, false);
    assertEq(err && typeof err === "object" && err.code, "send_failed");
});

await test("an sftp reply sent a byte at a time is assembled", async () => {
    const bytewise = (payload) => {
        const bytes = sftpCodec.frame(payload), out = [];
        for (let i = 0; i < bytes.length; i++) out.push(data(bytes.slice(i, i + 1)));
        return out.join("");
    };
    const [f] = await sftpOver(bytewise(wire.writer().byte(105).uint32(1)
        .raw(sftpCodec.encodeAttrs({ size: 77 })).build()), 64);
    const [a] = await f.stat("x");
    assertOk(a);
    assertEq(a.size, 77);
});

await test("the sftp window is granted back only as data is consumed", async () => {
    const ch = channel.new({ window: 1000 });
    ch.open = true; ch.remoteId = 0;
    ch.recvWindow = 100;              // 900 received
    assertEq(ch.windowAdjustment(900), null);
    const adj = ch.windowAdjustment(500);
    assertEq(adj !== null && adj !== undefined, true);
    assertEq(ch.recvWindow, 500);
});

// audit 5 ---------------------------------------------------------------------

// An idle channel (an sftp session) beside a reader (a long exec).
function idleAndReader() {
    const s = fakeStream("", 4);
    const t = transport.new(s, stubCrypto());
    const idle = channel.new({ id: 5 });
    idle.open = true; idle.remoteId = 9;
    t.channels[5] = idle;
    return [t, s, idle, channel.new({ id: 1 })];
}

await test("keepalives at an idle channel are answered, not queued (SSH-M1)", async () => {
    const [t, s, idle, reader] = idleAndReader();
    for (let i = 0; i < 1000; i++) {
        await t.route(reader, { type: "request", recipient: 5,
                                request: "keepalive@openssh.com", wantReply: true });
    }
    assertEq(idle.inbox.length, 0, "nothing queued:");
    assertEq(t.dead, undefined);
    let failures = 0;
    for (const ty of writtenTypes(s)) if (ty === 100) failures++;
    assertEq(failures, 1000, "every keepalive answered:");
});

await test("unknown requests without a reply are applied, not queued (SSH-M1)", async () => {
    const [t, , idle, reader] = idleAndReader();
    for (let i = 0; i < 10000; i++) {
        await t.route(reader, { type: "request", recipient: 5, request: "x", wantReply: false });
    }
    assertEq(idle.inbox.length, 0);
    assertEq(t.dead, undefined);
});

await test("eof, close and exit-status are queued once each for their owner (SSH-M1)", async () => {
    const [t, , idle, reader] = idleAndReader();
    for (let i = 0; i < 3; i++) {
        await t.route(reader, { type: "request", recipient: 5, request: "exit-status",
                                wantReply: false, exitStatus: 3 });
        await t.route(reader, { type: "eof", recipient: 5 });
        await t.route(reader, { type: "close", recipient: 5 });
    }
    assertEq(idle.inbox.length, 3, "one of each:");
    assertEq(idle.closed, true);
    assertEq(idle.exitStatus, 3);
    const seen = [];
    for (let i = 0; i < 3; i++) {
        const m = await t.readFor(idle);
        assertEq(m.routed, true);
        seen.push(m.type);
    }
    assertEq(seen[2], "close");
});

await test("a flood of replies kills the connection, not the unrelated reader (SSH-M1)", async () => {
    const [t, s, idle, reader] = idleAndReader();
    for (let i = 0; i < 64; i++) {
        await t.route(reader, { type: "request_success", recipient: 5 });
    }
    assertEq(t.dead, undefined, "64 replies are tolerated:");
    let ok = true, err;
    try { await t.route(reader, { type: "request_success", recipient: 5 }); }
    catch (e) { ok = false; err = e; }
    assertEq(ok, true, String(err));
    assertEq(t.dead && t.dead.code, "protocol_error");
    assertEq(s.closed, true);
    assertEq(idle.inbox.length, 64);
    let sok = true, serr;
    try { await t.sendPacket("\x02"); } catch (e) { sok = false; serr = e; }
    assertEq(sok, false);
    assertEq(serr && typeof serr === "object" && serr.code, "protocol_error");
});

await test("a stream call refused as busy leaves the flag to its owner (SSH-L1)", async () => {
    const s = fakeStream(plain("\x02x"), 64);
    const t = transport.new(s, stubCrypto());
    const orig = s.read;
    let nestedErr, busyDuring;
    s.read = async function (n) {
        if (nestedErr === undefined) {
            try { await t.streamCall("read", 4); nestedErr = null; } catch (e) { nestedErr = e; }
            busyDuring = t.ioBusy;
        }
        return orig.call(this, n);
    };
    await t.streamCall("read", 4);
    assertEq(nestedErr && typeof nestedErr === "object" && nestedErr.code, "busy", String(nestedErr));
    assertEq(busyDuring, true, "the owner's flag survives the refusal:");
    assertEq(t.ioBusy, false, "and is cleared by the owner:");
});

await test("a rekey refused as busy leaves the connection usable (SSH-L2)", async () => {
    const t = transport.new(fakeStream("", 4), stubCrypto());
    t.sessionId = "sid";
    t.ioBusy = true;          // another caller is parked in a stream call
    let ran = false;
    t.runKex = async () => { ran = true; return [true]; };
    const [ok, err] = await t.rekey();
    assertEq(ok, null);
    assertEq(err.code, "busy");
    assertEq(ran, false, "no exchange started:");
    assertEq(t.dead, undefined, "a busy refusal is not a failed exchange:");
    assertEq(!t.inKex, true, "in_kex untouched under the parked reader:");
    const due = { rekeyDue: () => true };
    t.c2s = due; t.s2c = due;
    assertEq(await t.maybeRekey(), false);
    assertEq(t.dead, undefined);
});

await test("a rekey under strict KEX tolerates IGNORE and DEBUG (SSH-L3)", async () => {
    const s = fakeStream(plain(b(2) + "pad")
        + plain(b(4) + zeros(12))
        + plain(b(21)), 4);
    const t = transport.new(s, stubCrypto());
    assertEq((await t.expect(21, "NEWKEYS", "rekey")).charCodeAt(0), 21);
    const open = wire.writer().byte(90).string("x11").uint32(1).uint32(0).uint32(0).build();
    const s2 = fakeStream(plain(open) + plain(b(21)), 4);
    const t2 = transport.new(s2, stubCrypto());
    const err = await assertRaises(() => t2.expect(21, "NEWKEYS", "rekey"));
    assertEq(has(err, "strict KEX"), true, err);
});

await test("run_kex applies the full strict rule only to the first exchange (SSH-L3)", async () => {
    const offer = offerWith({ kex: ["curve25519-sha256", kexinit.STRICT_S] });
    const server = kexinit.build(offer, zeros(16));
    const filterSeen = async (rekey) => {
        const t = transport.new(fakeStream("", 4), fullCrypto([]));
        if (rekey) { t.sessionId = "sid"; t.strictKex = true; }
        else t.recvSeq = 1;
        let seen;
        t.expect = async (_w, _what, strict) => { seen = strict; throw new Error("stop"); };
        try { await t.runKex({ offer: kexinit.DEFAULT_OFFER }, server); } catch (e) { /* stop */ }
        return seen;
    };
    assertEq(await filterSeen(false), true, "first exchange:");
    assertEq(await filterSeen(true), "rekey", "rekey:");
});

await test("message_ready puts the wait back when the read raises", async () => {
    const s = scriptedStream([]);
    const t = transport.new(s, stubCrypto(), { keepaliveMs: 30000 });
    t.ioBusy = true;
    assertEq(await raisedCode(() => t.messageReady()), "busy");
    assertEq(s.waits[s.waits.length - 1], 30000, "wait after the raise:");
});

// Interleaving at awaits (#1009 audit) --------------------------------------
// A Lua coroutine resumed from a stream call runs straight on; a JS caller
// resumes through awaits, with other promise jobs in between. These run a
// second caller in exactly that gap: a reaction on the very promise the
// stream call is waiting on runs right after the transport's own.

await test("a send in the gap after a write cannot reuse its sequence number", async () => {
    let release;
    const gate = new Promise((r) => { release = r; });
    const s = fakeStream("", 1);
    s.write = function (x) { this.written.push(x); this.p = gate.then(() => [true]); return this.p; };
    const t = transport.new(s, stubCrypto());
    const seqs = [];
    t.c2s = { seal: (aead, p, rnd, seq) => { seqs.push(seq); return "sealed" + String(seq); } };
    const a = t.sendPacket("A");
    let bDone = null, bErr = null;
    s.p.then(() => { bDone = t.sendPacket("B").catch((e) => { bErr = e; }); });
    release();
    await a;
    await gate; await null;
    if (bDone) await bDone;
    assertOk(bDone !== null, "the second send ran in the gap:");
    assertEq(new Set(seqs).size, seqs.length, "no sequence number sealed twice: " + seqs.join(","));
    assertEq(t.sendSeq, seqs.length, "every seal spent a number:");
    if (bErr) assertEq(bErr.code, "busy", "or the second send is refused:");
});

await test("bytes a fill already read survive a later read that throws", async () => {
    const s = scriptedStream(["abc"]);
    s.read = function (n) {
        const idx = this.i; this.i = idx + 1;
        if (idx === 0) return ["abc"];
        return [null, "another caller is waiting", "busy"];
    };
    const t = transport.new(s, stubCrypto());
    assertEq(await raisedCode(() => t.fill(6)), "busy");
    assertEq(t.inbuf, "abc", "the chunk read before the refusal is kept:");
});

await test("a second reader cannot take a packet under the first one's sequence number", async () => {
    const t = transport.new(fakeStream("", 1), stubCrypto());
    const seen = [];
    t.s2c = {
        open: (aead, buf, seq) => {
            seen.push(seq);
            if (buf.length < 4) return [null];
            return [buf.slice(0, 4), 4];
        },
        needed: () => 4,
    };
    t.inbuf = "AAAABBBB";
    const a = t.readPacket();
    const bErr = await raisedCode(() => t.readPacket());
    assertEq(await a, "AAAA");
    assertEq(bErr, "busy", "the second read is refused while the first holds the buffer:");
    assertEq(t.recvSeq, 1);
    assertEq(await t.readPacket(), "BBBB");
    assertEq(seen.join(","), "0,1", "each packet opened under its own number:");
});

await test("the connection's own busy failure is not a runtime refusal", async () => {
    const t = transport.new(fakeStream("", 1), stubCrypto());
    t.ioBusy = true;
    let err = null;
    try { await t.sendPacket("x"); } catch (e) { err = e; }
    assertEq(err && err.code, "busy");
    assertEq(Object.prototype.hasOwnProperty.call(err, "ssh"), true, "marked as the connection's:");
});

await test("deadlines nest: an inner operation puts back what is left of the outer one", async () => {
    const s = scriptedStream([]);
    let now = 0;
    const t = transport.new(s, stubCrypto(), { clock: () => now });
    const outer = t.pushDeadline(5000);        // an exec with timeoutMs 5000
    now = 1000;
    const inner = t.pushDeadline(60000);       // an SFTP call from its onStdout
    now = 2000;
    t.popDeadline(inner);
    t.popDeadline(outer);
    assertEq(s.deadlines.join(","), "5000,4000,3000,0",
             "the outer bound survives the inner one:");
});

await test("a cleanup bound holds alone, even past an expired outer one", async () => {
    const s = scriptedStream([]);
    let now = 0;
    const t = transport.new(s, stubCrypto(), { clock: () => now });
    const outer = t.pushDeadline(10);
    now = 100;
    const drain = t.pushDeadline(500, true);
    t.popDeadline(drain);
    t.popDeadline(outer);
    assertEq(s.deadlines.join(","), "10,500,1,0", "cleanup, then the expired outer:");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
