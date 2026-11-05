// test_ssh_chacha.js - Tests for hull:ssh:chacha (twin of test_ssh_chacha.lua)
//
// The chacha20-poly1305@openssh.com packet layer. The framing is checked with
// stand-in primitives, then the same cases run against hull:crypto's real
// ChaCha20 and Poly1305 (the Lua suite's caps-bearing leg; 17 cases in all).

import { chacha } from "hull:ssh:chacha";
import { packet } from "hull:ssh:packet";
import { wire } from "hull:ssh:wire";
import { bytes } from "hull:ssh:bytes";

let pass = 0, fail = 0;
async function test(name, fn) {
    try { await fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + (e && e.stack || e)); }
}

// A case that needs the mbedTLS backend (AES-CTR / GCM, ChaCha20, Poly1305,
// RSA, ECDSA). The JS twin of test_lua.c's #ifdef HL_LINK_TLS real-crypto
// legs: the C harness sets __hull_build from the build's own macros, so a case
// is skipped only in a build without TLS (hull:crypto throws "no ... backend"
// there, as Lua's does), never in one that has it.
const TLS = !globalThis.__hull_build || globalThis.__hull_build.tls !== false;
function realTest(name, fn) {
    if (TLS) return test(name, fn);
    console.log("SKIP: " + name + " (mbedTLS is not linked in this build)");
    return undefined;
}
function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}
function assertRaises(fn, want) {
    let err = null, ok = true;
    try { fn(); } catch (e) { ok = false; err = e; }
    if (ok) throw new Error("should have raised");
    const m = String(err && err.message !== undefined ? err.message : err);
    if (want && m.indexOf(want) < 0) throw new Error("raised " + m + ", wanted " + want);
}

// Stand-ins: output depends on key, nonce and counter. Not cryptography.
function toyStream(key, nonce, counter, data) {
    let seed = 0;
    for (let i = 0; i < key.length; i++) seed = (seed * 31 + key.charCodeAt(i)) % 2147483647;
    for (let i = 0; i < nonce.length; i++) seed = (seed * 31 + nonce.charCodeAt(i)) % 2147483647;
    const out = [];
    let x = (seed + counter * 64) % 2147483647;
    for (let i = 0; i < data.length; i++) {
        x = (x * 48271 + (i + 1)) % 2147483647;
        out.push(String.fromCharCode(data.charCodeAt(i) ^ (x & 0xFF)));
    }
    return out.join("");
}
function toyMac(key, msg) {
    let h = 0;
    for (let i = 0; i < key.length; i++) h = (h * 131 + key.charCodeAt(i)) % 4294967291;
    for (let i = 0; i < msg.length; i++) h = (h * 131 + msg.charCodeAt(i)) % 4294967291;
    const lo16 = h % 65536;
    const h2 = h - lo16 + (lo16 ^ 0x5555);
    return "\x00\x00\x00\x00" + wire.uint32(h) + "\x00\x00\x00\x00" + wire.uint32(h2);
}
const TOY = { chacha20: toyStream, poly1305: toyMac, ctEq: (a, b) => a === b };

const c = bytes.crypto;
const REAL = { chacha20: c.chacha20, poly1305: c.poly1305, ctEq: c.constantTimeEq };

const KEY = "\x01".repeat(32) + "\x02".repeat(32);
const zeros = (n) => "\x00".repeat(n);
const flip = (s, i) => s.slice(0, i) + String.fromCharCode(s.charCodeAt(i) ^ 1) + s.slice(i + 1);

for (const prims of [TOY, REAL]) {
    const tag = prims === TOY ? "[toy] " : "[real] ";
    const t = prims === TOY ? test : realTest;

    await t(tag + "a sealed packet opens under the same key and sequence number", () => {
        const a = chacha.new(KEY, prims), b = chacha.new(KEY, prims);
        const frame = a.seal(null, "hello, server", zeros, 7);
        assertEq(b.needed(frame, 7), frame.length);
        const [payload, used] = b.open(null, frame, 7);
        assertEq(payload, "hello, server");
        assertEq(used, frame.length);
    });

    await t(tag + "the length is encrypted, so the frame cannot be sized by peeking", () => {
        const frame = chacha.new(KEY, prims).seal(null, "x", zeros, 0);
        const plainLen = frame.length - chacha.LENGTH_LEN - chacha.TAG_LEN;
        assertEq(wire.peekUint32(frame) !== plainLen, true);
    });

    await t(tag + "another sequence number does not open it", () => {
        const frame = chacha.new(KEY, prims).seal(null, "payload", zeros, 3);
        assertRaises(() => chacha.new(KEY, prims).open(null, frame, 4));
    });

    await t(tag + "a flipped byte anywhere fails authentication", () => {
        const frame = chacha.new(KEY, prims).seal(null, "payload payload", zeros, 1);
        const bad = flip(frame, chacha.LENGTH_LEN + 2);   // Lua index LENGTH_LEN + 3
        assertRaises(() => chacha.new(KEY, prims).open(null, bad, 1), "failed authentication");
        const badtag = flip(frame, frame.length - 1);
        assertRaises(() => chacha.new(KEY, prims).open(null, badtag, 1), "failed authentication");
    });

    await t(tag + "the payload key and the length key are different halves", () => {
        const swapped = KEY.slice(32, 64) + KEY.slice(0, 32);
        const frame = chacha.new(KEY, prims).seal(null, "payload", zeros, 0);
        assertRaises(() => chacha.new(swapped, prims).open(null, frame, 0));
    });

    await t(tag + "an incomplete frame asks for more rather than failing", () => {
        const frame = chacha.new(KEY, prims).seal(null, "payload", zeros, 9);
        const r = chacha.new(KEY, prims);
        assertEq(r.open(null, frame.slice(0, 3), 9)[1], "need_more");
        assertEq(r.open(null, frame.slice(0, -1), 9)[1], "need_more");
        assertEq(r.needed(frame.slice(0, 3), 9), 4);
    });

    await t(tag + "padding aligns the encrypted region to 8, length excluded", () => {
        for (let n = 0; n <= 20; n++) {
            const frame = chacha.new(KEY, prims).seal(null, "p".repeat(n), zeros, 0);
            const region = frame.length - chacha.LENGTH_LEN - chacha.TAG_LEN;
            assertEq(region % 8, 0, "payload " + n + ":");
            assertEq(region - 1 - n >= packet.MIN_PADDING, true);
        }
    });
}

await test("an implausible decrypted length is refused before any wait", () => {
    const ch = chacha.new(KEY, TOY);
    const nonce = "\x00\x00\x00\x00" + "\x00\x00\x00\x00" + wire.uint32(5);
    const huge = toyStream(KEY.slice(32, 64), nonce, 0, wire.uint32(0x80000000));
    assertRaises(() => ch.needed(huge + zeros(40), 5), "exceeds the maximum");
    const odd = toyStream(KEY.slice(32, 64), nonce, 0, wire.uint32(13));
    assertRaises(() => ch.needed(odd + zeros(40), 5), "whole number of blocks");
});

await test("a key of the wrong size, or missing primitives, is refused", () => {
    assertRaises(() => chacha.new("k".repeat(32), TOY), "64 bytes");
    assertRaises(() => chacha.new(KEY, { chacha20: toyStream }));
});

await test("a sequence number is required", () => {
    assertRaises(() => chacha.new(KEY, TOY).seal(null, "x", zeros, null), "sequence number");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
