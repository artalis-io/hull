// test_encoding.js - Tests for hull:encoding
//
// The same RFC 4648 vectors and strictness rules as
// stdlib/lua/hull/tests/test_encoding.lua, so the two runtimes cannot drift
// apart silently; plus the buffer inputs only JS has.

import { encoding } from "hull:encoding";

const { hex, base64, base32, bytes } = encoding;

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + e.message);
    }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function assertThrows(fn, msg) {
    try { fn(); } catch (e) { return; }
    throw new Error((msg || "should have thrown") + " but did not");
}

let ALL = "";
for (let i = 0; i < 256; i++) ALL += String.fromCharCode(i);

const IN = ["", "f", "fo", "foo", "foob", "fooba", "foobar"];

// hex -------------------------------------------------------------------------

test("hex: RFC 4648 vectors, lowercase", () => {
    const want = ["", "66", "666f", "666f6f", "666f6f62", "666f6f6261", "666f6f626172"];
    IN.forEach((s, i) => assertEq(hex.encode(s), want[i], s));
    assertEq(hex.encode("\x00\x01\x7f\x80\xff"), "00017f80ff");
});

test("hex: every byte round trips, and either case decodes", () => {
    assertEq(hex.decode(hex.encode(ALL)), ALL);
    assertEq(hex.decode("00017F80FF"), "\x00\x01\x7f\x80\xff");
});

test("hex: strict decoding", () => {
    assertEq(hex.decode("abc"), null);
    assertEq(hex.decode("zz"), null);
    assertEq(hex.decode(" f"), null);
    assertEq(hex.decode("-f"), null);
    assertEq(hex.decode(""), "");
    assertThrows(() => hex.decode(null), "non-string");
    assertThrows(() => hex.encode(42), "non-bytes");
});

test("hex: buffers encode, characters above 0xFF are refused", () => {
    assertEq(hex.encode(new Uint8Array([0, 255]).buffer), "00ff");
    assertEq(hex.encode(new Uint8Array([9, 0, 255, 9]).subarray(1, 3)), "00ff");
    assertThrows(() => hex.encode("Ā"), "char above 0xFF");
});

// base64 ----------------------------------------------------------------------

test("base64: RFC 4648 vectors, padded and not", () => {
    const padded = ["", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"];
    const bare   = ["", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"];
    IN.forEach((s, i) => {
        assertEq(base64.encode(s), padded[i], s);
        assertEq(base64.encode(s, { pad: false }), bare[i], s);
        assertEq(base64.decode(padded[i]), s, "decode padded " + s);
        assertEq(base64.decode(bare[i]), s, "decode bare " + s);
    });
});

test("base64: url alphabet, unpadded by default", () => {
    assertEq(base64.encode("\xfb\xff\xbf", { url: true }), "-_-_");
    assertEq(base64.encode("\xfb\xff\xbf"), "+/+/");
    assertEq(base64.encode("f", { url: true }), "Zg");
    assertEq(base64.encode("f", { url: true, pad: true }), "Zg==");
    assertEq(base64.decode("-_-_", { url: true }), "\xfb\xff\xbf");
});

test("base64: every byte round trips in both alphabets", () => {
    assertEq(base64.decode(base64.encode(ALL)), ALL);
    assertEq(base64.decode(base64.encode(ALL, { url: true }), { url: true }), ALL);
});

test("base64: strict decoding", () => {
    assertEq(base64.decode("Zm9v!YmFy"), null);
    assertEq(base64.decode("Zm9v-YmFy"), null, "url char, std alphabet");
    assertEq(base64.decode("+/+/", { url: true }), null, "std char, url alphabet");
    assertEq(base64.decode("Zg==", { url: true }), null, "url takes no padding");
    assertEq(base64.decode("Zm9v\nYmFy"), null, "whitespace needs lenient");
    assertEq(base64.decode("Zg="), null);
    assertEq(base64.decode("Zm9v="), null);
    assertEq(base64.decode("Zg==="), null);
    assertEq(base64.decode("Zg==Zg=="), null, "data after padding");
    assertEq(base64.decode("Zm9vY"), null);
});

test("base64: lenient decoding skips whitespace only", () => {
    assertEq(base64.decode("Zm9v\nYmFy\r\n", { lenient: true }), "foobar");
    assertEq(base64.decode("Zm9v YmE=\n", { lenient: true }), "fooba");
    assertEq(base64.decode("Zm9v!YmFy", { lenient: true }), null);
});

// base32 ----------------------------------------------------------------------

test("base32: RFC 4648 vectors, unpadded", () => {
    const want = ["", "MY", "MZXQ", "MZXW6", "MZXW6YQ", "MZXW6YTB", "MZXW6YTBOI"];
    IN.forEach((s, i) => {
        assertEq(base32.encode(s), want[i], s);
        assertEq(base32.decode(want[i]), s, "decode " + s);
        assertEq(base32.decode(want[i].toLowerCase()), s, "lowercase " + s);
    });
});

test("base32: every byte round trips", () => {
    assertEq(base32.decode(base32.encode(ALL)), ALL);
});

test("base32: strict, and lenient for pasted secrets", () => {
    assertEq(base32.decode("MZXW6==="), null);
    assertEq(base32.decode("MZXW 6YTB"), null);
    assertEq(base32.decode("MZXW1"), null);
    assertEq(base32.decode("mzxw 6ytb oi==\n", { lenient: true }), "foobar");
    assertEq(base32.decode("MZXW1", { lenient: true }), null);
});

// utf8 ------------------------------------------------------------------------

test("utf8: text encodes to the bytes the Lua suite checks", () => {
    assertEq(encoding.utf8.encode("héllo"), "h\xc3\xa9llo");
    assertEq(encoding.utf8.encode("€"), "\xe2\x82\xac");
    assertEq(encoding.utf8.encode("\u{1F600}"), "\xf0\x9f\x98\x80");
    assertEq(encoding.utf8.encode(""), "");
    assertThrows(() => encoding.utf8.encode("a\ud800"), "lone high surrogate");
    assertThrows(() => encoding.utf8.encode("\udc00b"), "lone low surrogate");
});

test("utf8: well-formed bytes decode back to the text", () => {
    for (const t of ["héllo", "€", "\u{1F600}", "", "plain"]) {
        assertEq(encoding.utf8.decode(encoding.utf8.encode(t)), t, t);
    }
    assertEq(encoding.utf8.decode(new Uint8Array([0xe2, 0x82, 0xac])), "€", "buffer input");
});

test("utf8: malformed bytes do not decode", () => {
    for (const b of ["\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80", "ok\xff"]) {
        assertEq(encoding.utf8.decode(b), null, encoding.hex.encode(b));
    }
    assertThrows(() => encoding.utf8.decode(null), "non-bytes");
});

// bytes -----------------------------------------------------------------------

test("bytes: byte strings and buffers convert both ways", () => {
    const u8 = bytes.toU8("\x00\x80\xff");
    assertEq(u8.length, 3);
    assertEq(u8[1], 0x80);
    assertEq(bytes.fromBuffer(u8), "\x00\x80\xff");
    assertEq(bytes.fromBuffer(u8.buffer), "\x00\x80\xff");
    assertThrows(() => bytes.fromBuffer("x"), "string is not a buffer");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
