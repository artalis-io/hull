// test_ssh_tunnel.js - reaching a host through a WebSocket relay, and the
// public hull:ssh surface. The JS twin of test_ssh_tunnel.lua.
//
// The composition, not the protocol: ssh.connect must dial the RELAY, speak
// the upgrade over it with the caller's headers, and hand what comes back to
// the SSH transport unchanged. Driven against a fake relay; asserts on the
// bytes that relay was sent.
//
// JS shape: a failure Lua returns as (nil, err) is a rejection with a coded
// Error here; a Lua raise is a TypeError rejection.

import { ssh } from "hull:ssh";
import { wsStream as ws } from "hull:ssh:ws_stream";
import { wire } from "hull:ssh:wire";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try {
        await fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + ((e && e.stack) || e));
    }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}

function assertMatch(s, pat, msg) {
    if (String(s).indexOf(pat) < 0) {
        throw new Error((msg || "no match") + ": " + pat + " not in " + String(s));
    }
}

// The rejection of `p` (fails the test if it resolves).
async function rejects(p, msg) {
    try {
        await p;
    } catch (e) {
        return e;
    }
    throw new Error((msg || "should have rejected") + " but resolved");
}

const testHooks = __testHooks("hull:ssh");
function withTransport(fake, fn) {
    testHooks.setTransportNew(() => fake);
    return (async () => {
        try { return await fn(); } finally { testHooks.setTransportNew(null); }
    })();
}

// Deterministic stand-ins.
const stubRandom = (n) => "\x2a".repeat(n);
const stubSha1 = () => "\x07".repeat(20);
const cryptoStub = { random: stubRandom, sha1: stubSha1 };

// ssh-ed25519 public key blobs, which acceptHost checks for.
const edBlob = (byte) => wire.writer().string("ssh-ed25519").string(byte.repeat(32)).build();
const BLOB22 = edBlob("\x01"), BLOB2222 = edBlob("\x02");

function relay(inbound) {
    return {
        _in: inbound || "", _pos: 0, written: [], closed: false,
        async read(n) {
            if (this._pos >= this._in.length) return [""];
            const take = Math.min(n, this._in.length - this._pos);
            const s = this._in.slice(this._pos, this._pos + take);
            this._pos += take;
            return [s];
        },
        async write(s) { this.written.push(s); return [true]; },
        close() { this.closed = true; },
    };
}

function upgraded(body) {
    const accept = ws.accept(stubSha1, ws.key(stubRandom));
    return "HTTP/1.1 101 Switching Protocols\r\n"
        + "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        + "Sec-WebSocket-Accept: " + accept + "\r\n\r\n" + (body || "");
}

// Connect through a fake relay -> [error or null, what the opener saw].
async function connectVia(r, tunnel) {
    let seen;
    try {
        await ssh.connect({
            host: "spark-7468", user: "operator", key: {}, crypto: cryptoStub,
            tunnel: tunnel || { host: "ssh.example.com" },
            openStream: (o) => { seen = o; return r; },
        });
    } catch (e) {
        return [e, seen];
    }
    return [null, seen];
}

await test("the relay is dialled, not the SSH host", async () => {
    const r = relay(upgraded());
    const [, seen] = await connectVia(r);
    assertEq(seen.host, "spark-7468");
    assertEq(seen.port, 22);
    assertEq(seen.user, "operator");
    assertEq(seen.via.host, "ssh.example.com");
    assertEq(seen.via.port, 443);
    assertEq(seen.via.tls, true, "a relay carries credentials; TLS is the default");
});

await test("tls = false is honoured, for a local shim", async () => {
    const r = relay(upgraded());
    const [, seen] = await connectVia(r, { host: "127.0.0.1", port: 8080, tls: false });
    assertEq(seen.via.tls, false);
    assertEq(seen.via.port, 8080);
});

await test("the upgrade request carries the caller's headers verbatim", async () => {
    const r = relay(upgraded());
    await connectVia(r, {
        host: "ssh.example.com",
        headers: [
            "Cf-Access-Client-Id: abc.access",
            "Cf-Access-Client-Secret: shh",
            "Cf-Access-Jump-Destination: spark-7468:22",
        ],
    });
    const req = r.written[0] || "";
    assertMatch(req, "GET / HTTP/1.1\r\n");
    assertMatch(req, "Host: ssh.example.com\r\n");
    assertMatch(req, "Upgrade: websocket\r\n");
    assertMatch(req, "Sec-WebSocket-Version: 13\r\n");
    assertMatch(req, "Cf-Access-Client-Id: abc.access\r\n");
    assertMatch(req, "Cf-Access-Client-Secret: shh\r\n");
    assertMatch(req, "Cf-Access-Jump-Destination: spark-7468:22\r\n");
});

// The destination header ------------------------------------------------------

function count(s, sub) {
    let n = 0, i = 0;
    for (;;) {
        const j = s.indexOf(sub, i);
        if (j < 0) return n;
        n++;
        i = j + sub.length;
    }
}

await test("the destination header is written from the granted host", async () => {
    const r = relay(upgraded());
    await connectVia(r, { host: "ssh.example.com", headers: ["Cf-Access-Client-Id: abc.access"] });
    assertMatch(r.written[0] || "", "Cf-Access-Jump-Destination: spark-7468:22\r\n");
});

await test("a destination header naming another host is refused before dialling", async () => {
    const r = relay(upgraded());
    const [err, seen] = await connectVia(r, { host: "ssh.example.com",
        headers: ["cf-access-jump-destination: db-prod:22"] });
    assertEq(err.code, "denied");
    assertMatch(err.detail, "granted for spark-7468:22");
    assertEq(seen, undefined, "nothing may be dialled");
});

await test("a destination header on another port is refused too", async () => {
    const r = relay(upgraded());
    const [err] = await connectVia(r, { host: "ssh.example.com",
        headers: ["Cf-Access-Jump-Destination: spark-7468:2222"] });
    assertEq(err.code, "denied");
});

await test("a matching destination header is kept once, not doubled", async () => {
    const r = relay(upgraded());
    await connectVia(r, { host: "ssh.example.com",
        headers: ["CF-ACCESS-JUMP-DESTINATION:  spark-7468:22 "] });
    const req = r.written[0] || "";
    assertEq(count(req.toLowerCase(), "cf-access-jump-destination:"), 1);
});

await test("a relay that routes by another header names it", async () => {
    const r = relay(upgraded());
    await connectVia(r, { host: "ssh.example.com", destinationHeader: "X-Target" });
    const req = r.written[0] || "";
    assertMatch(req, "X-Target: spark-7468:22\r\n");
    assertEq(req.indexOf("Cf-Access-Jump-Destination"), -1);
});

await test("destination_header = false sends none, as the caller said", async () => {
    const r = relay(upgraded());
    await connectVia(r, { host: "ssh.example.com", destinationHeader: false });
    assertEq((r.written[0] || "").indexOf("Jump-Destination"), -1);
});

await test("the well-known destination header is checked even when another is configured", async () => {
    for (const dh of [false, "X-Target"]) {
        const r = relay(upgraded());
        const [err, seen] = await connectVia(r, { host: "ssh.example.com",
            destinationHeader: dh,
            headers: ["Cf-Access-Jump-Destination: db-prod:22"] });
        assertEq(err && err.code, "denied");
        assertEq(seen, undefined, "nothing may be dialled");
    }
});

await test("tunnel.headers may not set the upgrade's own headers", async () => {
    for (const h of ["Host: other.zone.example", "upgrade: x",
                     "Connection: close", "Sec-WebSocket-Key: AAAA"]) {
        const r = relay(upgraded());
        const [err, seen] = await connectVia(r, { host: "ssh.example.com", headers: [h] });
        assertEq(err && err.code, "denied", h);
        assertEq(seen, undefined, "nothing may be dialled: " + h);
    }
});

await test("tunnel.path is origin-form only", async () => {
    for (const p of ["http://internal.example/", "//internal.example/",
                     "relay", "/a b", "/a\tb"]) {
        const [e] = await connectVia(relay(upgraded()), { host: "ssh.example.com", path: p });
        assertEq(e instanceof TypeError, true, p);
        assertMatch(String(e.message), "tunnel.path", p);
    }
    const r = relay(upgraded());
    await connectVia(r, { host: "ssh.example.com", path: "/ssh?x=1" });
    assertMatch(r.written[0] || "", "GET /ssh?x=1 HTTP/1.1\r\n");
});

await test("tunnel.headers entries are header lines", async () => {
    for (const h of ["no colon here", " evil:22", ": empty", "Bad Name: x", "\tX-A: b"]) {
        const r = relay(upgraded());
        const [err, seen] = await connectVia(r, { host: "ssh.example.com",
            headers: ["Cf-Access-Jump-Destination: spark-7468:22", h] });
        assertEq(err && err.code, "denied", h);
        assertEq(seen, undefined, "nothing may be dialled: " + h);
    }
});

await test("a header that is not a primitive string is refused, line breaks and all (#1009 audit)", async () => {
    const forged = new String("X-A: 1\r\nCf-Access-Jump-Destination: db-prod:22");
    const objs = [forged, { toString: () => "X-A: 1\r\nHost: evil" }, 7];
    for (const h of objs) {
        const r = relay(upgraded());
        const [err, seen] = await connectVia(r, { host: "ssh.example.com", headers: [h] });
        assertEq(err instanceof TypeError, true, "a caller error: " + String(err));
        assertEq(seen, undefined, "nothing may be dialled");
        assertEq(r.written.length, 0, "nothing may be written");
    }
    // And the request builder itself refuses one, whoever calls it.
    let threw = false;
    try {
        ws.buildRequest({ host: "r", key: "k", headers: [forged] });
    } catch (e) {
        threw = e instanceof TypeError;
    }
    assertEq(threw, true, "buildRequest refuses a non-string header:");
});

await test("the tunnel options are read once: the Host header names the host the grant saw", async () => {
    let n = 0;
    const tunnel = {
        get host() { n++; return n === 1 ? "ssh.example.com" : "other-origin.example"; },
    };
    const r = relay(upgraded());
    const [, seen] = await connectVia(r, tunnel);
    assertEq(seen.via.host, "ssh.example.com");
    assertMatch(r.written[0] || "", "Host: ssh.example.com\r\n");
    assertEq((r.written[0] || "").indexOf("other-origin") < 0, true, "the second answer is never used:");
});

await test("ssh_tunnel_audit44.write_refusal_unchanged", async () => {
    // The relay's stream refuses every write after the upgrade the way the
    // runtime refuses a wait inside a transaction. The refusal says nothing
    // about the connection: it must reach the caller as it was thrown, not as
    // io_error text.
    const refusal = new TypeError("hull:ssh: cannot wait inside a transaction");
    refusal.code = "wait_refused";
    const ident = String.fromCharCode(0x80 | ws.OP_BIN, 14) + "SSH-2.0-Fake\r\n";
    const r = relay(upgraded(ident));
    let writes = 0;
    r.write = async function (s) {
        writes++;
        if (writes > 1) throw refusal;
        this.written.push(s);
        return [true];
    };
    const [e] = await connectVia(r, { host: "ssh.example.com" });
    assertEq(e, refusal, "connect rethrows the refusal unchanged:");

    // And the stream itself, under any caller.
    const r2 = relay(upgraded());
    const [s] = await ws.connect(r2, { host: "ssh.example.com", random: stubRandom,
                                       sha1: stubSha1 });
    r2.write = async () => { throw refusal; };
    const e2 = await rejects(s.write("abc"), "write");
    assertEq(e2, refusal, "write rethrows it unchanged:");
});

await test("ssh_audit44.banner_callback_error_reraised", async () => {
    // The caller's own onBanner throwing is the caller's bug, not the
    // connection's: it comes back as thrown, like onStdout's, and the stream
    // is closed.
    const r = relay("a banner line\r\nSSH-2.0-Fake\r\n");
    const e = await rejects(ssh.connect({
        host: "spark-7468", user: "operator", key: {}, crypto: cryptoStub,
        openStream: () => r,
        onBanner: () => { throw "my bug"; },
    }), "connect");
    assertEq(e, "my bug", "the callback's own error:");
    assertEq(r.closed, true, "the stream is closed:");
});

await test("ssh_audit44.pending_trust_not_spent_by_bad_args", async () => {
    // A connect refused for its own arguments dials nothing, so it must not
    // use up the one connection a pending trust is for.
    const pt = ssh.pendingTrust();
    const tryConnect = (o) => {
        o.host = "spark-7468"; o.user = "operator"; o.key = {};
        o.crypto = cryptoStub; o.trust = pt;
        o.openStream = o.openStream || (() => { throw new Error("nothing may be dialled"); });
        return ssh.connect(o);
    };
    for (const o of [{ port: "22" }, { timeoutMs: 1.5 },
                     { tunnel: { host: "ssh.example.com", port: "443" } },
                     { tunnel: { host: "ssh.example.com", path: "relay" } },
                     { software: "Hull\r\nX" }]) {
        await rejects(tryConnect(o), "a bad argument rejects");
    }
    const e2 = await rejects(tryConnect({ openStream: () => relay("") }), "dialled");
    assertEq(e2.code !== "already_used", true, "the trust is still unused: " + e2.code);
    const e3 = await rejects(tryConnect({ openStream: () => relay("") }), "again");
    assertEq(e3.code, "already_used", "and the dialled connect spent it:");
});

await test("ssh_tunnel_audit44.upgrade_refusal_closes_raw", async () => {
    // The runtime refuses the upgrade's own write (a wait inside a
    // transaction): the refusal reaches the caller unchanged, and the relay
    // stream is closed then, not left open until it is collected.
    const refusal = new TypeError("hull:ssh: cannot wait inside a transaction");
    refusal.code = "wait_refused";
    const r = relay(upgraded());
    r.write = async () => { throw refusal; };
    const [e] = await connectVia(r, { host: "ssh.example.com" });
    assertEq(e, refusal, "unchanged:");
    assertEq(r.closed, true, "the relay stream is closed:");
});

await test("the upgrade runs under the connect deadline (#1009 review)", async () => {
    const r = relay(upgraded());
    const log = [];
    r.deadline = (ms) => { log.push("deadline:" + String(ms)); };
    const write = r.write;
    r.write = async function (x) { log.push("write"); return write.call(this, x); };
    await connectVia(r);
    assertEq(log[0], "deadline:30000", "bounded before the request goes out: " + log.join(" "));
    assertEq(log.indexOf("write") > 0, true);
});

await test("an IPv6 destination is bracketed", async () => {
    const r = relay(upgraded());
    await rejects(ssh.connect({
        host: "fd00::7", user: "operator", key: {}, crypto: cryptoStub,
        tunnel: { host: "ssh.example.com" },
        openStream: () => r,
    }));
    assertMatch(r.written[0] || "", "Cf-Access-Jump-Destination: [fd00::7]:22\r\n");
});

await test("ports and timeouts are integers, not numeric strings (audit 39)", async () => {
    const tryIt = (o) => {
        o.host = o.host || "spark-7468"; o.user = "operator"; o.key = {};
        o.crypto = cryptoStub;
        o.tunnel = o.tunnel || { host: "ssh.example.com" };
        o.openStream = () => { throw new Error("nothing may be dialled"); };
        return rejects(ssh.connect(o));
    };
    // PARITY: Lua's { port = 22.0 } (a float) has no JS twin - 22.0 IS the
    // integer 22 in JS - so that case is replaced by a fractional port.
    for (const o of [{ port: "22e2" }, { port: "22" }, { port: 22.5 },
                     { timeoutMs: "1000" },
                     { tunnel: { host: "ssh.example.com", port: "443" } }]) {
        const e = await tryIt(o);
        assertMatch(String(e.message), "must be an integer");
    }
    const r = relay(upgraded());
    const [, seen] = await connectVia(r, { host: "ssh.example.com" });
    assertEq(Number.isInteger(seen.port), true);
    const r2 = relay(upgraded());
    await rejects(ssh.connect({
        host: "spark-7468", port: 2200, user: "operator", key: {},
        crypto: cryptoStub, tunnel: { host: "ssh.example.com" },
        openStream: () => r2,
    }));
    assertMatch(r2.written[0] || "", "Cf-Access-Jump-Destination: spark-7468:2200\r\n");
});

await test("a destination that is not one host name is refused before dialling (audit 39)", async () => {
    for (const h of ["evil,a.example.com", "a.example.com x", "a\r\nX: y"]) {
        let seen;
        const err = await rejects(ssh.connect({
            host: h, user: "operator", key: {}, crypto: cryptoStub,
            tunnel: { host: "ssh.example.com" },
            openStream: (o) => { seen = o; return relay(upgraded()); },
        }));
        assertEq(err && err.code, "denied", h);
        assertEq(seen, undefined, "nothing may be dialled: " + h);
    }
});

await test("a destination_header that is not a name is a caller error", async () => {
    const e = await rejects(ssh.connect({
        host: "spark-7468", user: "operator", key: {}, crypto: cryptoStub,
        tunnel: { host: "ssh.example.com", destinationHeader: 7 },
    }));
    assertEq(e instanceof TypeError, true);
});

// The connection handle ------------------------------------------------------

function connectWithFakeTransport() {
    const fake = {
        hostFingerprint: "SHA256:fake",
        handshake: () => [true],
        setDeadline: () => {},
        authenticate: () => [true],
        close: () => {},
        sendPacket: () => { throw new Error("must not be reachable"); },
        exec: (c) => [{ status: 0, stdout: c, stderr: "" }],
        sftp() {
            return [{ t: this, read: (p) => ["contents of " + p] }];
        },
    };
    return withTransport(fake, () => ssh.connect({
        host: "spark-7468", user: "operator", key: {}, crypto: cryptoStub,
        openStream: () => ({}),
    }));
}

await test("a connection exposes its methods, not its transport", async () => {
    const conn = await connectWithFakeTransport();
    assertEq(conn.t, undefined, "conn.t");
    assertEq(Object.keys(conn).length, 0, "the handle has no fields at all");
    // PARITY: Lua's getmetatable(conn) == false; JS's prototype is a frozen
    // table of methods, and the handle itself is frozen.
    assertEq(Object.isFrozen(conn), true);
    assertEq(Object.isFrozen(Object.getPrototypeOf(conn)), true);
    assertEq((await conn.exec("uptime")).stdout, "uptime", "the methods still work:");
    assertEq(conn.fingerprint(), "SHA256:fake");
});

await test("an sftp session exposes its methods, not the transport under it", async () => {
    const conn = await connectWithFakeTransport();
    const f = await conn.sftp();
    assertEq(f.t, undefined, "sftp.t");
    assertEq(Object.keys(f).length, 0, "the handle has no fields at all");
    assertEq(await f.read("/etc/motd"), "contents of /etc/motd");
});

await test("connect hands the port to the host-key check", async () => {
    let seen;
    await rejects(withTransport({
        handshake: (o) => { seen = o; return [null, { code: "stop" }]; },
        setDeadline: () => {},
        close: () => {},
    }, () => ssh.connect({ host: "spark-7468", port: 2222, user: "operator",
                           key: {}, crypto: cryptoStub, openStream: () => ({}) })));
    assertEq(seen && seen.port, 2222);
});

await test("accept_host and forget_host name the entry by host and port", async () => {
    const trust = ssh.memoryStore();
    await ssh.acceptHost(trust, "Spark-7468", BLOB22);
    await ssh.acceptHost(trust, "spark-7468", BLOB2222, 2222);
    const e = trust.entries();
    assertEq(e["spark-7468"], BLOB22);
    assertEq(e["[spark-7468]:2222"], BLOB2222);
    await ssh.forgetHost(trust, "spark-7468", 2222);
    assertEq(trust.entries()["[spark-7468]:2222"], undefined);
    assertEq(trust.entries()["spark-7468"], BLOB22, "port 22 untouched:");
});

// The error model --------------------------------------------------------------

function connOver(fakeMethods) {
    const fake = {
        handshake: () => [true],
        authenticate: () => [true],
        setDeadline: () => {},
        close: () => {},
    };
    for (const k of Object.keys(fakeMethods)) fake[k] = fakeMethods[k];
    return withTransport(fake, () => ssh.connect({
        host: "spark-7468", user: "operator", key: {}, crypto: cryptoStub,
        openStream: () => ({}),
    }));
}

await test("a transport failure inside exec comes back as io_error, not a raise", async () => {
    const conn = await connOver({ exec: () => { throw new Error("ssh: read failed: reset"); } });
    const err = await rejects(conn.exec("uptime"));
    assertEq(err.code, "io_error");
    assertMatch(err.detail, "read failed");
});

await test("a coded failure keeps its code", async () => {
    const conn = await connOver({ exec: () => {
        throw { code: "timeout", detail: "nothing from the server" }; } });
    const err = await rejects(conn.exec("uptime"));
    assertEq(err.code, "timeout");
});

await test("the caller's own callback error is raised, unchanged", async () => {
    const conn = await connOver({ async exec(_c, o) {
        try { await o.onStdout("x"); } catch (e) { throw { appError: e }; }
    } });
    const e = await rejects(conn.exec("x", { onStdout: () => { throw "my bug"; } }));
    assertEq(e, "my bug");
});

await test("a runtime refusal is rethrown unchanged", async () => {
    // PARITY: the JS form of a refusal (an Error with an own `code`,
    // hull:_refusal); Lua's twin is the "[hull:wait_refused] " string.
    const refusal = new TypeError("hull.sleep() cannot wait inside a transaction");
    refusal.code = "wait_refused";
    const conn = await connOver({ exec: () => { throw refusal; } });
    const e = await rejects(conn.exec("x"));
    assertEq(e, refusal);
});

await test("an sftp file is a handle too, and its errors come back coded", async () => {
    const file = {
        s: "the session", handle: "h",
        read: () => { throw new Error("ssh: read failed: reset"); },
        write: () => [true],
        close: () => [true],
    };
    const conn = await connOver({ sftp: () => [{ t: "the transport", open: () => [file] }] });
    const f = await (await conn.sftp()).open("/x", "r");
    assertEq(Object.keys(f).length, 0, "no fields on the handle:");
    assertEq(Object.isFrozen(f), true);
    assertEq(await f.write("data"), true);
    const err = await rejects(f.read(10));
    assertEq(err.code, "io_error");
});

await test("an unreadable key comes back as bad_key, not a raise", async () => {
    const err = await rejects(ssh.connect({ host: "h", user: "u", key: "not a key",
        crypto: cryptoStub, openStream: () => { throw new Error("must not dial"); } }));
    assertEq(err.code, "bad_key");
});

await test("exactly one of key and keys, and keys is a non-empty list", async () => {
    const base = { host: "h", user: "u", crypto: cryptoStub,
                   openStream: () => { throw new Error("must not dial"); } };
    const tryIt = async (extra) => {
        const e = await rejects(ssh.connect(Object.assign({}, base, extra)));
        assertEq(e instanceof TypeError, true, JSON.stringify(Object.keys(extra)));
    };
    await tryIt({});
    await tryIt({ key: {}, keys: [{}] });
    await tryIt({ keys: [] });
    await tryIt({ keys: "not a list" });
});

await test("a bad key anywhere in keys fails before the dial, and says which", async () => {
    const err = await rejects(ssh.connect({ host: "h", user: "u", keys: [{}, "not a key"],
        crypto: cryptoStub, openStream: () => { throw new Error("must not dial"); } }));
    assertEq(err.code, "bad_key");
    assertEq(err.key, 1, "0-based (Lua: 2)");
});

await test("a key in another tool's format is unsupported_key_type", async () => {
    const err = await rejects(ssh.connect({ host: "h", user: "u",
        key: "-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n",
        crypto: cryptoStub,
        openStream: () => { throw new Error("must not dial"); } }));
    assertEq(err.code, "unsupported_key_type");
});

await test("an unusable software string is bad_software, before any dial", async () => {
    for (const sw of ["", "Hull 1.0", "Hull\r\nX", 7]) {
        const err = await rejects(ssh.connect({ host: "h", user: "u", key: {}, software: sw,
            crypto: cryptoStub,
            openStream: () => { throw new Error("must not dial"); } }));
        assertEq(err.code, "bad_software", String(sw));
    }
});

await test("a key already trusted is an answer, not a raise", async () => {
    const trust = ssh.memoryStore();
    assertEq(await ssh.acceptHost(trust, "h", BLOB22), true);
    const err = await rejects(ssh.acceptHost(trust, "h", BLOB2222));
    assertEq(err.code, "already_trusted");
});

await test("a host that cannot be reached is connect_failed, not denied", async () => {
    const err = await rejects(ssh.connect({ host: "h", user: "u", key: {}, crypto: cryptoStub,
        openStream: () => [null, "connection refused", "connect_failed"] }));
    assertEq(err.code, "connect_failed");
    const err2 = await rejects(ssh.connect({ host: "h", user: "u", key: {}, crypto: cryptoStub,
        openStream: () => [null, "timed out", "timeout"] }));
    assertEq(err2.code, "timeout");
});

await test("the handshake runs under the connect budget, reported as timeout", async () => {
    let budget;
    const err = await rejects(withTransport({
        setDeadline: (ms) => { if (budget === undefined) budget = ms; },
        handshake: () => { throw { code: "deadline" }; },
        close: () => {},
    }, () => ssh.connect({ host: "h", user: "u", key: {}, crypto: cryptoStub,
                           timeoutMs: 4000, openStream: () => ({}) })));
    assertEq(budget, 4000);
    assertEq(err.code, "timeout");
    assertMatch(err.detail, "4000 ms");
});

await test("a refused upgrade reports the status, not a manifest denial", async () => {
    const r = relay("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
    const [err] = await connectVia(r);
    assertEq(err.code, "upgrade_refused");
    assertEq(err.status, 403);
    assertEq(r.closed, true, "the socket is ours to drop when the upgrade fails");
});

await test("a relay that is not a WebSocket server is not mistaken for one", async () => {
    const r = relay("HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: wrong\r\n\r\n");
    const [err] = await connectVia(r);
    assertEq(err.code, "upgrade_failed");
    assertEq(r.closed, true);
});

await test("a dial that is refused keeps the refusal", async () => {
    const err = await rejects(ssh.connect({
        host: "spark-7468", user: "operator", key: {}, crypto: cryptoStub,
        tunnel: { host: "ssh.example.com" },
        openStream: () => [null, "tunnel host is not in ssh.tunnel.hosts", "denied"],
    }));
    assertEq(err.code, "denied");
    assertMatch(err.detail, "ssh.tunnel.hosts");
});

await test("a tunnel without a host is refused before anything is dialled", async () => {
    let dialled = false;
    const e = await rejects(ssh.connect({
        host: "h", user: "u", key: {}, crypto: cryptoStub,
        tunnel: { port: 443 },
        openStream: () => { dialled = true; return relay(upgraded()); },
    }), "a tunnel with no host should raise");
    assertEq(e instanceof TypeError, true);
    assertEq(dialled, false);
});

await test("bytes from inside the tunnel reach the SSH transport", async () => {
    const frame = String.fromCharCode(0x80 | ws.OP_BIN, 16) + "SSH-2.0-Fake\r\n\x00\x00";
    const r = relay(upgraded(frame));
    const [err] = await connectVia(r);
    assertEq(err !== null, true, "a truncated conversation still fails");
    assertEq(err.code !== "upgrade_failed", true, "but not at the upgrade");
    assertEq(err.code !== "upgrade_refused", true);
    assertEq(err.code !== "denied", true);
});

// The dial's errno ---------------------------------------------------------------
//
// The binding's fourth value is the socket error's symbolic name; the stdlib
// hands it to the app as `e.errno`, so a health check branches on a value and
// never on `detail`. The stub stands in for the binding: EHOSTUNREACH is not
// reproducible portably, and the hint is stdlib text in any case.

const LOCAL_NET = "Privacy & Security > Local Network";
async function dialFails(errnoName, extra) {
    const o = Object.assign({ host: "h", user: "u", key: {}, crypto: cryptoStub,
        openStream: () => [null, "connection refused or unreachable: " + String(errnoName)
                                 + " (No route to host)", "connect_failed", errnoName] },
        extra || {});
    return rejects(ssh.connect(o));
}

await test("connect_failed carries the errno name, and the hint for a link-local literal", async () => {
    const err = await dialFails("EHOSTUNREACH", { host: "169.254.1.2" });
    assertEq(err.code, "connect_failed");
    assertEq(err.errno, "EHOSTUNREACH");
    assertMatch(err.detail, "EHOSTUNREACH (No route to host)", "the C text is kept");
    assertMatch(err.detail, LOCAL_NET, "the macOS hint follows it");
    assertMatch(err.detail, "after every hull build");
    assertEq(err.detail.indexOf("169.254"), -1, "no host is added to detail");
});

await test("the hint is for EHOSTUNREACH to a private or link-local literal only", async () => {
    for (const h of ["10.0.0.7", "172.16.5.5", "172.31.255.1", "192.168.1.1",
                     "[fe80::1%en0]", "fd12:3456::1", "FC00::1"]) {
        const err = await dialFails("EHOSTUNREACH", { host: h });
        assertEq(err.errno, "EHOSTUNREACH");
        assertMatch(err.detail, LOCAL_NET, h);
    }
    for (const h of ["203.0.113.5", "172.32.0.1", "172.15.0.1", "8.8.8.8",
                     "2001:db8::1", "h", "router.local", "999.168.1.1"]) {
        const err = await dialFails("EHOSTUNREACH", { host: h });
        assertEq(err.errno, "EHOSTUNREACH");
        assertEq(err.detail.indexOf(LOCAL_NET), -1, h + " gets no hint");
    }
    // A refusal on a private address is a closed port, not privacy.
    const err = await dialFails("ECONNREFUSED", { host: "192.168.1.1" });
    assertEq(err.errno, "ECONNREFUSED");
    assertEq(err.detail.indexOf(LOCAL_NET), -1);
});

await test("a dial that reports no errno (DNS, TLS) sets none", async () => {
    const err = await rejects(ssh.connect({ host: "h", user: "u", key: {}, crypto: cryptoStub,
        openStream: () => [null, "host did not resolve", "connect_failed"] }));
    assertEq(err.code, "connect_failed");
    assertEq(err.errno, undefined);
    assertEq(err.detail, "host did not resolve");
});

await test("through a tunnel the errno is the relay's, and the hint reads the relay host", async () => {
    const err = await rejects(ssh.connect({ host: "h", user: "u", key: {}, crypto: cryptoStub,
        tunnel: { host: "fe80::1" },
        openStream: () => [null, "no route", "connect_failed", "EHOSTUNREACH"] }));
    assertEq(err.code, "connect_failed");
    assertEq(err.errno, "EHOSTUNREACH");
    assertMatch(err.detail, LOCAL_NET);
    // The SSH destination is a name here; the relay literal decides.
    const err2 = await rejects(ssh.connect({ host: "169.254.1.2", user: "u", key: {},
        crypto: cryptoStub, tunnel: { host: "relay.example" },
        openStream: () => [null, "no route", "connect_failed", "EHOSTUNREACH"] }));
    assertEq(err2.errno, "EHOSTUNREACH");
    assertEq(err2.detail.indexOf(LOCAL_NET), -1, "a relay NAME gets no hint");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
