// test_ssh_bytes.js - Tests for hull:ssh:bytes, and the hull:crypto primitives
// it adapts (gcmSeal / gcmOpen, aes256ctr, chacha20, poly1305, bcryptPbkdf),
// against published vectors. The JS twin of the crypto half of the Lua SSH
// suites' real-crypto legs.

import { bytes } from "hull:ssh:bytes";
import { crypto } from "hull:crypto";
import { encoding } from "hull:encoding";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + (e && e.message));
    }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function assertThrows(fn, msg) {
    let threw = false;
    try { fn(); } catch (e) { threw = true; }
    if (!threw) throw new Error((msg || "should have thrown") + " but did not");
}

const hex = (s) => encoding.hex.encode(s);
const unhex = (h) => encoding.hex.decode(h);
const c = bytes.crypto;

test("toU8 / fromBuf round trip every byte value", () => {
    let s = "";
    for (let i = 0; i < 256; i++) s += String.fromCharCode(i);
    const u = bytes.toU8(s);
    assertEq(u.length, 256);
    assertEq(u[255], 255);
    assertEq(bytes.fromBuf(u), s);
    assertEq(bytes.fromBuf(u.buffer), s);
});

test("fromBuf of a large buffer (past one apply chunk)", () => {
    const u = new Uint8Array(20000);
    for (let i = 0; i < u.length; i++) u[i] = i & 255;
    const s = bytes.fromBuf(u);
    assertEq(s.length, 20000);
    assertEq(s.charCodeAt(19999), 19999 & 255);
});

test("toU8 refuses a character above 0xFF", () => {
    assertThrows(() => bytes.toU8("Ā"));
    assertEq(bytes.isBytes("ÿ"), true);
    assertEq(bytes.isBytes("Ā"), false);
});

test("sha256 / sha1 are raw digests of the bytes", () => {
    assertEq(hex(c.sha256("abc")),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assertEq(hex(c.sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
    // a high byte is ONE byte, not its UTF-8 form
    assertEq(c.sha256("\xff").length, 32);
    assertEq(hex(c.sha256("\xff")), hex(bytes.fromBuf(crypto.sha256(new Uint8Array([255])))));
});

test("aes256ctr: NIST SP 800-38A F.5.5", () => {
    const key = unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
    const iv = unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const pt = unhex("6bc1bee22e409f96e93d7e117393172a");
    assertEq(hex(c.aes256ctr(key, iv, pt)), "601ec313775789a5b7a7f504bbf3d228");
    assertEq(c.aes256ctr(key, iv, c.aes256ctr(key, iv, pt)), pt);
    assertEq(c.aes256ctr(key, iv, ""), "");
    assertThrows(() => c.aes256ctr(key.slice(1), iv, pt), "short key");
    assertThrows(() => c.aes256ctr(key, iv.slice(1), pt), "short iv");
});

test("chacha20: RFC 8439 2.4.2", () => {
    const key = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const nonce = unhex("000000000000004a00000000");
    const pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    const ct = c.chacha20(key, nonce, 1, pt);
    assertEq(hex(ct).slice(0, 32), "6e2e359a2568f98041ba0728dd0d6981");
    assertEq(c.chacha20(key, nonce, 1, ct), pt);
    assertThrows(() => c.chacha20(key, nonce, -1, pt), "negative counter");
    assertThrows(() => c.chacha20(key, nonce, 2 ** 32, pt), "counter past 2^32-1");
});

test("poly1305: RFC 8439 2.5.2", () => {
    const key = unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    assertEq(hex(c.poly1305(key, "Cryptographic Forum Research Group")),
        "a8061dc1305136c6c22b8baf0c0127a9");
});

test("gcm: seal / open round trip, tamper and wrong-length tag fail as null", () => {
    const key = c.random(32), iv = c.random(12);
    const [ct, tag] = c.gcmSeal(key, iv, "aad", "hello, ssh");
    assertEq(ct.length, 10);
    assertEq(tag.length, 16);
    assertEq(c.gcmOpen(key, iv, "aad", ct, tag), "hello, ssh");
    const bad = String.fromCharCode(tag.charCodeAt(0) ^ 1) + tag.slice(1);
    assertEq(c.gcmOpen(key, iv, "aad", ct, bad), null);
    assertEq(c.gcmOpen(key, iv, "aad", ct, tag.slice(1)), null);
    assertEq(c.gcmOpen(key, iv, "AAD", ct, tag), null);
    const [e, et] = c.gcmSeal(key, iv, "", "");
    assertEq(e, "");
    assertEq(c.gcmOpen(key, iv, "", "", et), "");
});

test("gcm: NIST test case 14 (zero key, zero block)", () => {
    const key = unhex("0000000000000000000000000000000000000000000000000000000000000000");
    const iv = unhex("000000000000000000000000");
    const [ct, tag] = c.gcmSeal(key, iv, "", unhex("00000000000000000000000000000000"));
    assertEq(hex(ct), "cea7403d4d606b6e074ec5d3baf39d18");
    assertEq(hex(tag), "d0d1c8a799996bf0265b98b5d48ab919");
});

test("bcryptPbkdf: the OpenBSD regress vector", () => {
    assertEq(hex(c.bcryptPbkdf("password", "salt", 4, 32)),
        "5bbf0cc293587f1c3635555c27796598d47e579071bf427e9d8fbe842aba34d9");
    assertThrows(() => c.bcryptPbkdf("", "salt", 4, 32), "empty passphrase");
    assertThrows(() => c.bcryptPbkdf("pw", "", 4, 32), "empty salt");
    assertThrows(() => c.bcryptPbkdf("pw", "salt", 0, 32), "zero rounds");
    assertThrows(() => c.bcryptPbkdf("pw", "salt", c.BCRYPT_MAX_ROUNDS + 1, 32), "too many rounds");
    assertThrows(() => c.bcryptPbkdf("pw", "salt", 4, 0), "zero length");
    assertThrows(() => c.bcryptPbkdf("pw", "salt", 1.5, 32), "fractional rounds");
});

test("bcryptPbkdfEnv: an undeclared variable is refused by name", () => {
    let msg = "";
    try { c.bcryptPbkdfEnv("HULL_SSH_NOPE_" + "X", "salt", 4, 32); } catch (e) { msg = e.message; }
    if (msg.indexOf("is not available") < 0) throw new Error("got: " + msg);
});

test("x25519 / ed25519 through the adapter", () => {
    const [pa, sa] = c.x25519Keypair();
    const [pb, sb] = c.x25519Keypair();
    assertEq(pa.length, 32);
    const [k1] = c.x25519(sa, pb);
    const [k2] = c.x25519(sb, pa);
    assertEq(k1, k2);
    const [low, why] = c.x25519(sa, "\0".repeat(32));
    assertEq(low, null);
    assertEq(typeof why, "string");
    const kp = crypto.ed25519Keypair();
    const sk = bytes.fromBuf(kp.secretKey), pk = bytes.fromBuf(kp.publicKey);
    const sig = c.ed25519Sign("msg\xff", sk);
    assertEq(c.ed25519Verify("msg\xff", sig, pk), true);
    assertEq(c.ed25519Verify("msg\xfe", sig, pk), false);
});

test("constantTimeEq / hmacSha1", () => {
    assertEq(c.constantTimeEq("\x01\x02", "\x01\x02"), true);
    assertEq(c.constantTimeEq("\x01\x02", "\x01\x03"), false);
    assertEq(c.constantTimeEq("\x01", "\x01\x02"), false);
    // RFC 2202 test case 2
    assertEq(hex(c.hmacSha1("what do ya want for nothing?", "Jefe")),
        "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
