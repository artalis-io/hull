// test_ssh_cipher.js - Tests for hull:ssh:cipher (twin of test_ssh_cipher.lua)
//
// The IV cases are why this file exists: the counter advances exactly once per
// packet, it is never reachable by a caller, and the two directions never
// share one. A last section runs the round trip under the real AES-256-GCM
// (hull:ssh:bytes' crypto), which Lua pins in test_crypto.c.

import { cipher } from "hull:ssh:cipher";
import { wire as W } from "hull:ssh:wire";
import { bytes } from "hull:ssh:bytes";

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

const KEY = "\x01".repeat(32);
const IV = "\x00".repeat(12);
const zeros = (n) => "\x00".repeat(n);
const TAG = "\x07".repeat(16);

// A stand-in AEAD that records what it was handed.
function fakeAead(log) {
    return {
        seal(key, iv, aad, plain) {
            if (log) log.push({ op: "seal", iv, aad });
            const out = [];
            for (let i = 0; i < plain.length; i++) {
                out.push(String.fromCharCode((plain.charCodeAt(i) + iv.charCodeAt(i % 12)) % 256));
            }
            return [out.join(""), TAG];
        },
        open(key, iv, aad, ct, tag) {
            if (log) log.push({ op: "open", iv, aad });
            if (tag !== TAG) return null;
            const out = [];
            for (let i = 0; i < ct.length; i++) {
                out.push(String.fromCharCode((ct.charCodeAt(i) - iv.charCodeAt(i % 12) + 256) % 256));
            }
            return out.join("");
        },
    };
}

// construction ---------------------------------------------------------------

await test("key and iv lengths are enforced", () => {
    assertRaises(() => cipher.new("short", IV), "short key");
    assertRaises(() => cipher.new(KEY, "short"), "short iv");
    assertRaises(() => cipher.new(null, IV), "no key");
});

await test("the iv splits into a fixed part and a counter", () => {
    const c = cipher.new(KEY, "\x01\x02\x03\x04" + "\x00\x00\x00\x00\x00\x00\x00\x09");
    assertEq(c.iv(), "\x01\x02\x03\x04\x00\x00\x00\x00\x00\x00\x00\x09");
});

// round trip ------------------------------------------------------------------

await test("seal and open round trip", () => {
    const aead = fakeAead();
    const enc = cipher.new(KEY, IV);
    const dec = cipher.new(KEY, IV);
    for (const payload of ["", "x", "abc".repeat(200)]) {
        const w = enc.seal(aead, payload, zeros);
        const [got, used] = dec.open(aead, w);
        assertEq(got, payload);
        assertEq(used, w.length);
    }
});

await test("the length field travels in the clear", () => {
    const c = cipher.new(KEY, IV);
    const w = c.seal(fakeAead(), "hello", zeros);
    assertEq(W.peekUint32(w), w.length - 4 - 16, "length excludes itself and the tag");
});

await test("the encrypted region is block aligned", () => {
    for (let n = 0; n <= 40; n++) {
        const c = cipher.new(KEY, IV);
        const w = c.seal(fakeAead(), "x".repeat(n), zeros);
        assertEq(W.peekUint32(w) % 16, 0, "aligned for payload " + n);
    }
});

await test("the length is authenticated as associated data", () => {
    const log = [];
    const c = cipher.new(KEY, IV);
    const w = c.seal(fakeAead(log), "hello", zeros);
    assertEq(log[0].aad, w.slice(0, 4), "AAD is the length field");
});

// the IV counter ---------------------------------------------------------------

await test("the counter advances exactly once per packet", () => {
    const log = [];
    const aead = fakeAead(log);
    const c = cipher.new(KEY, IV);
    c.seal(aead, "one", zeros);
    c.seal(aead, "two", zeros);
    c.seal(aead, "three", zeros);
    assertEq(log.length, 3);
    assertEq(log[0].iv, "\x00\x00\x00\x00" + "\x00\x00\x00\x00\x00\x00\x00\x00");
    assertEq(log[1].iv, "\x00\x00\x00\x00" + "\x00\x00\x00\x00\x00\x00\x00\x01");
    assertEq(log[2].iv, "\x00\x00\x00\x00" + "\x00\x00\x00\x00\x00\x00\x00\x02");
});

await test("no IV is ever repeated across many packets", () => {
    const log = [];
    const aead = fakeAead(log);
    const c = cipher.new(KEY, IV);
    for (let i = 1; i <= 500; i++) c.seal(aead, "p" + i, zeros);
    const seen = new Set();
    for (const e of log) {
        assertEq(seen.has(e.iv), false, "IV repeated");
        seen.add(e.iv);
    }
    assertEq(c.packets, 500);
});

await test("the counter carries across a byte boundary", () => {
    const log = [];
    const aead = fakeAead(log);
    const c = cipher.new(KEY, "\x00\x00\x00\x00" + "\x00\x00\x00\x00\x00\x00\x00\xff");
    c.seal(aead, "a", zeros);
    c.seal(aead, "b", zeros);
    assertEq(log[0].iv.slice(4), "\x00\x00\x00\x00\x00\x00\x00\xff");
    assertEq(log[1].iv.slice(4), "\x00\x00\x00\x00\x00\x00\x01\x00", "carried into the next byte");
});

await test("the fixed part never moves", () => {
    const log = [];
    const aead = fakeAead(log);
    const c = cipher.new(KEY, "\x09\x08\x07\x06" + "\x00" + "\xff".repeat(7));
    c.seal(aead, "a", zeros);
    c.seal(aead, "b", zeros);
    assertEq(log[0].iv.slice(0, 4), "\x09\x08\x07\x06");
    assertEq(log[1].iv.slice(0, 4), "\x09\x08\x07\x06", "unchanged after a full carry");
    assertEq(log[1].iv.slice(4), "\x01\x00\x00\x00\x00\x00\x00\x00", "carry rippled correctly");
});

await test("a wrapped counter raises rather than reusing an IV", () => {
    const c = cipher.new(KEY, "\x00\x00\x00\x00" + "\xff".repeat(8));
    assertRaises(() => c.seal(fakeAead(), "x", zeros), "wrap");
});

await test("opening advances the counter too", () => {
    const aead = fakeAead();
    const enc = cipher.new(KEY, IV);
    const dec = cipher.new(KEY, IV);
    const a = enc.seal(aead, "first", zeros);
    const b = enc.seal(aead, "second", zeros);
    assertEq(dec.open(aead, a)[0], "first");
    assertEq(dec.open(aead, b)[0], "second");
});

await test("the two directions do not share a counter", () => {
    const aead = fakeAead([]);
    const c2s = cipher.new(KEY, IV);
    const s2c = cipher.new(KEY, IV);
    c2s.seal(aead, "a", zeros);
    s2c.seal(aead, "b", zeros);
    assertEq(c2s.packets, 1);
    assertEq(s2c.packets, 1);
});

// partial reads and hostile input --------------------------------------------------

await test("open asks for more while the packet is incomplete", () => {
    const aead = fakeAead();
    const enc = cipher.new(KEY, IV);
    const w = enc.seal(aead, "hello world", zeros);
    for (let n = 0; n <= w.length - 1; n++) {
        const dec = cipher.new(KEY, IV);
        const [got, err] = dec.open(aead, w.slice(0, n));
        assertEq(got, null, "partial " + n);
        assertEq(err, "need_more");
        assertEq(dec.packets, 0, "an incomplete packet must not advance the IV");
    }
});

await test("a failed tag raises and does not advance", () => {
    const aead = fakeAead();
    const enc = cipher.new(KEY, IV);
    const w = enc.seal(aead, "hello", zeros);
    const tampered = w.slice(0, -1) + "\x00";
    const dec = cipher.new(KEY, IV);
    assertRaises(() => dec.open(aead, tampered), "bad tag");
    assertEq(dec.packets, 0);
});

await test("an implausible length is refused before any buffering", () => {
    const dec = cipher.new(KEY, IV);
    assertRaises(() => dec.open(fakeAead(), "\xff\xff\xff\xff" + "\x00".repeat(40)), "huge length");
});

await test("a non-block-multiple length is refused", () => {
    const dec = cipher.new(KEY, IV);
    assertRaises(() => dec.open(fakeAead(), W.uint32(17) + "\x00".repeat(60)), "misaligned");
    assertRaises(() => dec.open(fakeAead(), W.uint32(0) + "\x00".repeat(60)), "zero length");
});

await test("needed refuses the lengths open refuses", () => {
    const dec = cipher.new(KEY, IV);
    assertRaises(() => dec.needed("\xff\xff\xff\xff"), "huge length");
    assertRaises(() => dec.needed(W.uint32(17)), "misaligned");
    assertEq(dec.needed(W.uint32(32)), cipher.frameSize(32), "valid:");
    assertEq(dec.needed("\x00\x00"), cipher.LENGTH_LEN, "short:");
});

await test("padding that does not fit is refused", () => {
    const aead = {
        seal: () => ["", ""],
        open: () => String.fromCharCode(200) + "x".repeat(15),
    };
    const dec = cipher.new(KEY, IV);
    assertRaises(() => dec.open(aead, W.uint32(16) + "\x00".repeat(32)), "padding exceeds packet");
});

await test("seal demands real randomness", () => {
    assertRaises(() => cipher.new(KEY, IV).seal(fakeAead(), "x", null), "no rng");
    assertRaises(() => cipher.new(KEY, IV).seal(fakeAead(), "x", () => "short"), "wrong length");
});

// the rekey limits (RFC 4253 section 9) --------------------------------------

await test("bytes are counted as they go on the wire, frame and tag included", () => {
    const enc = cipher.new(KEY, IV);
    const fr = enc.seal(fakeAead(), "hello", zeros);
    assertEq(enc.bytesProcessed(), fr.length, "sealed:");
    const dec = cipher.new(KEY, IV);
    dec.open(fakeAead(), fr);
    assertEq(dec.bytesProcessed(), fr.length, "opened:");
});

await test("bytes accumulate across packets", () => {
    const enc = cipher.new(KEY, IV);
    let total = 0;
    for (let i = 0; i < 5; i++) total += enc.seal(fakeAead(), "abc", zeros).length;
    assertEq(enc.bytesProcessed(), total);
});

await test("a fresh cipher is not due for a rekey", () => {
    assertEq(cipher.new(KEY, IV).rekeyDue(), false);
});

await test("either limit on its own makes a rekey due", () => {
    const c = cipher.new(KEY, IV);
    c.seal(fakeAead(), "x", zeros);
    assertEq(c.rekeyDue({ bytes: 1, packets: Infinity }), true, "bytes:");
    assertEq(c.rekeyDue({ bytes: Infinity, packets: 1 }), true, "packets:");
    assertEq(c.rekeyDue({ bytes: Infinity, packets: Infinity }), false, "neither:");
});

await test("the default limits sit well inside the hard backstop", () => {
    assertEq(cipher.REKEY_PACKETS < cipher.MAX_PACKETS, true, "packets:");
    assertEq(cipher.REKEY_BYTES > 0, true, "bytes:");
});

// the real AES-256-GCM (JS only: Lua pins it in test_crypto.c) --------------

const realAead = {
    seal: (k, iv, aad, p) => bytes.crypto.gcmSeal(k, iv, aad, p),
    open: (k, iv, aad, c, t) => bytes.crypto.gcmOpen(k, iv, aad, c, t),
};

await test("real crypto: seal and open round trip, tamper refused", () => {
    const key = bytes.crypto.random(32), iv = bytes.crypto.random(12);
    const enc = cipher.new(key, iv), dec = cipher.new(key, iv);
    for (const payload of ["", "x", "\xff\x00".repeat(300)]) {
        const w = enc.seal(realAead, payload, bytes.crypto.random);
        const [got, used] = dec.open(realAead, w);
        assertEq(got, payload);
        assertEq(used, w.length);
    }
    const w = enc.seal(realAead, "secret", bytes.crypto.random);
    const t = w.slice(0, 10) + String.fromCharCode(w.charCodeAt(10) ^ 1) + w.slice(11);
    assertRaises(() => dec.open(realAead, t), "tampered");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
