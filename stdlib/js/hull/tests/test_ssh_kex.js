// test_ssh_kex.js - Tests for hull:ssh:kex (twin of test_ssh_kex.lua)
//
// RFC 8731 (curve25519-sha256) and RFC 4253 section 7.2 (key derivation).
// The orderings are what these tests are for.

import { kex } from "hull:ssh:kex";
import { wire } from "hull:ssh:wire";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try { await fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + (e && e.stack || e)); }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}

function assertRaises(fn, msg) {
    let ok = true;
    try { fn(); } catch (e) { ok = false; }
    if (ok) throw new Error((msg || "should have raised") + " but did not");
}

const Q = "\x01".repeat(32);    // a stand-in 32-byte point

// messages --------------------------------------------------------------

await test("build_ecdh_init carries the point as a string", () => {
    const r = wire.reader(kex.buildEcdhInit(Q));
    assertEq(r.byte(), kex.SSH_MSG_KEX_ECDH_INIT);
    assertEq(r.string(), Q);
    assertEq(r.remaining(), 0);
});

await test("build_ecdh_init demands a 32-byte point", () => {
    assertRaises(() => kex.buildEcdhInit("short"), "short");
    assertRaises(() => kex.buildEcdhInit("x".repeat(33)), "long");
});

await test("parse_ecdh_reply returns the three fields", () => {
    const p = wire.writer()
        .byte(kex.SSH_MSG_KEX_ECDH_REPLY)
        .string("hostkeyblob")
        .string(Q)
        .string("signature")
        .build();
    const got = kex.parseEcdhReply(p);
    assertEq(got.hostKey, "hostkeyblob");
    assertEq(got.qS, Q);
    assertEq(got.signature, "signature");
});

await test("parse_ecdh_reply rejects a point of the wrong length", () => {
    const p = wire.writer()
        .byte(kex.SSH_MSG_KEX_ECDH_REPLY)
        .string("k")
        .string("\x01".repeat(31))
        .string("s")
        .build();
    assertRaises(() => kex.parseEcdhReply(p), "31-byte point");
});

await test("parse_ecdh_reply rejects the wrong message type", () => {
    const p = wire.writer().byte(30).string("k").string(Q).string("s").build();
    assertRaises(() => kex.parseEcdhReply(p), "wrong type");
});

await test("parse_ecdh_reply rejects a truncated reply", () => {
    const p = wire.writer().byte(kex.SSH_MSG_KEX_ECDH_REPLY).string("k").build();
    assertRaises(() => kex.parseEcdhReply(p), "truncated");
});

// exchange hash ----------------------------------------------------------

function fields() {
    return {
        vC: "SSH-2.0-Client", vS: "SSH-2.0-Server",
        iC: "ICICIC", iS: "ISISIS",
        kS: "HOSTKEY", qC: "QQQQC", qS: "QQQQS",
        k: "\x2a",
    };
}

await test("exchange hash input is the RFC 8731 order", () => {
    const f = fields();
    const r = wire.reader(kex.exchangeHashInput(f));
    assertEq(r.string(), f.vC);
    assertEq(r.string(), f.vS);
    assertEq(r.string(), f.iC);
    assertEq(r.string(), f.iS);
    assertEq(r.string(), f.kS);
    assertEq(r.string(), f.qC);
    assertEq(r.string(), f.qS);
    assertEq(r.mpint(), f.k);
    assertEq(r.remaining(), 0);
});

await test("the shared secret is an mpint, not a string", () => {
    const f = fields();
    f.k = "\x80";                     // high bit set: mpint must pad it
    const blob = kex.exchangeHashInput(f);
    assertEq(blob.slice(-6), "\x00\x00\x00\x02\x00\x80");
});

await test("swapping two fields changes the bytes", () => {
    const a = kex.exchangeHashInput(fields());
    const f = fields();
    [f.qC, f.qS] = [f.qS, f.qC];
    const b = kex.exchangeHashInput(f);
    assertEq(a !== b, true, "transposed Q_C/Q_S must not hash the same");
});

await test("exchange hash refuses a missing field", () => {
    for (const name of ["vC", "vS", "iC", "iS", "kS", "qC", "qS", "k"]) {
        const f = fields();
        delete f[name];
        assertRaises(() => kex.exchangeHashInput(f), "missing " + name);
    }
});

// key derivation -----------------------------------------------------------

// A stand-in hash: deterministic and dependent on every input byte.
function fakeHash(data) {
    let acc = 0;
    const out = [];
    for (let i = 0; i < data.length; i++) {
        acc = (acc * 31 + data.charCodeAt(i)) % 4294967296;
    }
    for (let i = 0; i < 8; i++) {
        // (acc * 1103515245 + 12345) mod 2^32 without losing precision
        acc = (Number((BigInt(acc) * 1103515245n + 12345n) % 4294967296n));
        out.push(String.fromCharCode(acc % 256));
    }
    return out.join("");          // 8 bytes per round
}

await test("derive_key produces exactly the requested length", () => {
    for (const want of [1, 8, 12, 16, 32, 64]) {
        const k = kex.deriveKey(fakeHash, "K", "H", "S", "A", want);
        assertEq(k.length, want, "length for " + want);
    }
});

await test("derive_key chains when one round is not enough", () => {
    const one = kex.deriveKey(fakeHash, "K", "H", "S", "A", 8);
    const four = kex.deriveKey(fakeHash, "K", "H", "S", "A", 32);
    assertEq(four.slice(0, 8), one, "the first round is reused, not recomputed");
});

await test("each key id derives different material", () => {
    const seen = new Map();
    for (const id of Object.values(kex.KEY_IDS)) {
        const k = kex.deriveKey(fakeHash, "K", "H", "S", id, 16);
        assertEq(seen.get(k), undefined, "key id " + id + " collided");
        seen.set(k, id);
    }
});

await test("derive_key rejects a bad id", () => {
    assertRaises(() => kex.deriveKey(fakeHash, "K", "H", "S", "AB", 8), "two-letter id");
    assertRaises(() => kex.deriveKey(null, "K", "H", "S", "A", 8), "no hash function");
});

await test("derive_keys fills the sizes for aes256-gcm", () => {
    const sizes = kex.SIZES["aes256-gcm@openssh.com"];
    const keys = kex.deriveKeys(fakeHash, "\x01\x02\x03", "H", "S", sizes);
    assertEq(keys.ivC2s.length, 12);
    assertEq(keys.ivS2c.length, 12);
    assertEq(keys.keyC2s.length, 32);
    assertEq(keys.keyS2c.length, 32);
    assertEq(keys.macC2s, undefined);
    assertEq(keys.macS2c, undefined);
});

await test("the two directions never share key material", () => {
    const sizes = kex.SIZES["aes256-gcm@openssh.com"];
    const keys = kex.deriveKeys(fakeHash, "\x01\x02\x03", "H", "S", sizes);
    assertEq(keys.keyC2s !== keys.keyS2c, true, "encryption keys");
    assertEq(keys.ivC2s !== keys.ivS2c, true, "initial IVs");
});

await test("a different shared secret gives different keys", () => {
    const sizes = kex.SIZES["aes256-gcm@openssh.com"];
    const a = kex.deriveKeys(fakeHash, "\x01", "H", "S", sizes);
    const b = kex.deriveKeys(fakeHash, "\x02", "H", "S", sizes);
    assertEq(a.keyC2s !== b.keyC2s, true);
});

await test("the shared secret enters the KDF as an mpint", () => {
    const viaKeys = kex.deriveKeys(fakeHash, "\x80", "H", "S", { keyC2s: 8 }).keyC2s;
    const kMpint = wire.writer().mpint("\x80").build();
    const direct = kex.deriveKey(fakeHash, kMpint, "H", "S", "C", 8);
    assertEq(viaKeys, direct);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
