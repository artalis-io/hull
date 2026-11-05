// test_ssh_wire.js - Tests for hull:ssh:wire (the JS twin of test_ssh_wire.lua)
//
// The SSH binary data types (RFC 4251 section 5). These bytes arrive from an
// unauthenticated peer, so the truncation and malformed-input cases matter as
// much as the round trips.

import { wire } from "hull:ssh:wire";
import { encoding } from "hull:encoding";

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

const hex = encoding.hex.encode;
const ch = (...c) => String.fromCharCode(...c);
// A code point as its UTF-8 byte string (Lua's utf8.char).
const u8 = (cp) => encoding.utf8.encode(String.fromCodePoint(cp));
const utf8 = (s) => encoding.utf8.encode(s);

// round trips --------------------------------------------------------

await test("byte round trip", () => {
    const b = wire.writer().byte(0).byte(127).byte(255).build();
    assertEq(b.length, 3);
    const r = wire.reader(b);
    assertEq(r.byte(), 0);
    assertEq(r.byte(), 127);
    assertEq(r.byte(), 255);
    assertEq(r.remaining(), 0);
});

await test("uint32 is big endian", () => {
    const b = wire.writer().uint32(0x01020304).build();
    assertEq(hex(b), "01020304");
    assertEq(wire.reader(b).uint32(), 0x01020304);
});

await test("uint32 boundary values", () => {
    for (const v of [0, 1, 0xFFFFFFFF]) {
        const b = wire.writer().uint32(v).build();
        assertEq(wire.reader(b).uint32(), v);
    }
});

await test("uint64 round trip", () => {
    const b = wire.writer().uint64(0).uint64(4294967296).build();
    const r = wire.reader(b);
    assertEq(r.uint64(), 0);
    assertEq(r.uint64(), 4294967296);
});

await test("string is length prefixed and binary safe", () => {
    const payload = "a\x00b\xffc";
    const b = wire.writer().string(payload).build();
    assertEq(hex(b.slice(0, 4)), "00000005");
    assertEq(wire.reader(b).string(), payload);
});

await test("empty string round trip", () => {
    const b = wire.writer().string("").build();
    assertEq(hex(b), "00000000");
    assertEq(wire.reader(b).string(), "");
});

await test("boolean round trip", () => {
    const b = wire.writer().boolean(true).boolean(false).build();
    assertEq(hex(b), "0100");
    const r = wire.reader(b);
    assertEq(r.boolean(), true);
    assertEq(r.boolean(), false);
});

await test("any non-zero byte reads as true", () => {
    assertEq(wire.reader("\x02").boolean(), true);
    assertEq(wire.reader("\xff").boolean(), true);
});

// mpint ---------------------------------------------------------------

await test("mpint zero is an empty string", () => {
    const b = wire.writer().mpint("").build();
    assertEq(hex(b), "00000000");
    assertEq(wire.reader(b).mpint(), "");
    assertEq(hex(wire.writer().mpint("\x00\x00").build()), "00000000");
});

await test("mpint pads a high bit so it stays positive", () => {
    const b = wire.writer().mpint("\x80").build();
    assertEq(hex(b), "000000020080");
    assertEq(hex(wire.reader(b).mpint()), "80");
});

await test("mpint does not pad when the high bit is clear", () => {
    const b = wire.writer().mpint("\x7f").build();
    assertEq(hex(b), "000000017f");
});

await test("mpint strips unnecessary leading zeros", () => {
    const b = wire.writer().mpint("\x00\x00\x01\x02").build();
    assertEq(hex(b), "000000020102");
});

await test("mpint reader rejects a negative value", () => {
    const b = "\x00\x00\x00\x01\x80";
    assertRaises(() => wire.reader(b).mpint(), "negative mpint");
});

// name lists ----------------------------------------------------------

await test("namelist round trip", () => {
    const b = wire.writer().namelist(["ssh-ed25519", "rsa-sha2-256"]).build();
    assertEq(wire.reader(b).string(), "ssh-ed25519,rsa-sha2-256");
    const got = wire.reader(b).namelist();
    assertEq(got.length, 2);
    assertEq(got[0], "ssh-ed25519");
    assertEq(got[1], "rsa-sha2-256");
});

await test("empty namelist is legal", () => {
    const b = wire.writer().namelist([]).build();
    assertEq(hex(b), "00000000");
    assertEq(wire.reader(b).namelist().length, 0);
});

await test("namelist refuses a comma inside a name", () => {
    assertRaises(() => wire.writer().namelist(["a,b"]), "comma in name");
});

await test("namelist reader rejects an empty name", () => {
    for (const s of ["a,,b", ",a", "a,"]) {
        assertRaises(() => wire.reader(wire.string(s)).namelist(), "empty name in " + s);
    }
});

// truncation ----------------------------------------------------------

await test("every reader raises on truncation", () => {
    assertRaises(() => wire.reader("").byte(), "byte");
    assertRaises(() => wire.reader("\x01\x02").uint32(), "uint32");
    assertRaises(() => wire.reader("\x01\x02\x03\x04").uint64(), "uint64");
    assertRaises(() => wire.reader("\x00\x00\x00\x0aabc").string(), "string");
    assertRaises(() => wire.reader("\x00\x00\x00\x0aab").mpint(), "mpint");
    assertRaises(() => wire.reader("ab").raw(5), "raw");
});

await test("a huge claimed length does not allocate", () => {
    const b = "\xff\xff\xff\xff" + "short";
    assertRaises(() => wire.reader(b).string(), "huge length");
});

await test("reader tracks its own position", () => {
    const b = wire.writer().byte(1).string("hi").byte(2).build();
    const r = wire.reader(b);
    assertEq(r.remaining(), b.length);
    assertEq(r.byte(), 1);
    assertEq(r.string(), "hi");
    assertEq(r.byte(), 2);
    assertEq(r.remaining(), 0);
});

// writer validation ----------------------------------------------------

await test("writer rejects out-of-range values", () => {
    assertRaises(() => wire.writer().byte(256), "byte 256");
    assertRaises(() => wire.writer().byte(-1), "byte -1");
    assertRaises(() => wire.writer().uint32(-1), "uint32 -1");
    assertRaises(() => wire.writer().uint32(0x100000000), "uint32 big");
    assertRaises(() => wire.writer().uint64(-1), "uint64 -1");
    assertRaises(() => wire.writer().string(42), "string of number");
});

await test("a built packet composes in order", () => {
    const b = wire.writer()
        .byte(20)
        .string("cookie")
        .namelist(["curve25519-sha256"])
        .boolean(false)
        .uint32(0)
        .build();
    const r = wire.reader(b);
    assertEq(r.byte(), 20);
    assertEq(r.string(), "cookie");
    assertEq(r.namelist()[0], "curve25519-sha256");
    assertEq(r.boolean(), false);
    assertEq(r.uint32(), 0);
    assertEq(r.remaining(), 0);
});

// peer text headed for a terminal ---------------------------------------

await test("a namelist entry with an escape is refused, not cleaned", () => {
    const hostile = "aes256-gcm@openssh.com,ssh-\x1b[2Jrsa";
    const r = wire.reader(wire.string(hostile));
    let ok = true, err = null;
    try { r.namelist(); } catch (e) { ok = false; err = e; }
    assertEq(ok, false, "should have refused");
    assertEq(String(err && err.message).indexOf("printable") >= 0, true, String(err));
});

await test("ordinary algorithm names still parse", () => {
    const r = wire.reader(wire.string("curve25519-sha256,ext-info-c"));
    const list = r.namelist();
    assertEq(list.length, 2);
    assertEq(list[0], "curve25519-sha256");
});

await test("safe_text strips CR, which alone can overwrite a line", () => {
    assertEq(wire.safeText("ok" + ch(13) + "FAILED"), "okFAILED");
});

await test("safe_text keeps the layout characters a banner needs", () => {
    const s = "one" + ch(10) + "two" + ch(9) + "three";
    assertEq(wire.safeText(s), s);
});

await test("safe_name drops newline and tab as well", () => {
    assertEq(wire.safeName("a" + ch(10) + "b" + ch(9) + "c"), "abc");
});

await test("safe_name bounds what a peer can put in one error line", () => {
    const out = wire.safeName("x".repeat(500));
    assertEq(out.length <= 67, true, "got " + out.length);
    assertEq(out.slice(-3), "...");
});

await test("safe_name leaves ordinary text alone", () => {
    assertEq(wire.safeName("ssh-ed25519"), "ssh-ed25519");
});

await test("a raw 8-bit CSI byte is dropped", () => {
    const s = "ok" + ch(0x9B) + "2J";
    assertEq(wire.safeText(s), "ok2J");
    assertEq(wire.safeName(s), "ok2J");
});

await test("C1 controls encoded as UTF-8 are dropped", () => {
    assertEq(wire.safeText("a" + u8(0x9B) + "b" + u8(0x85) + "c"), "abc");
});

await test("directional overrides are dropped", () => {
    const s = "file" + u8(0x202E) + "txt.exe" + u8(0x2066) + "x" + u8(0x200F);
    assertEq(wire.safeName(s, 200), "filetxt.exex");
});

// JS hands back DECODED text (Lua keeps the bytes): a kept multi-byte
// character as byte-string code units would be UTF-8-encoded again when
// logged, and a continuation byte 0x80-0x9F became a real C1 control.
await test("ordinary non-ASCII text is kept intact, as decoded text", () => {
    const s = utf8("Grüße, 日本, ") + u8(0x1F600);
    const want = "Grüße, 日本, \u{1F600}";
    assertEq(wire.safeText(s), want);
    assertEq(wire.safeName(s, 200), want);
});

await test("a kept character never yields a C1 code unit (#1009 review)", () => {
    // U+00DB is C3 9B in UTF-8: kept byte for byte it was U+009B, CSI.
    const out = wire.safeText(utf8("Û") + utf8("€"));
    assertEq(out, "Û€");
    for (let i = 0; i < out.length; i++) {
        const c = out.charCodeAt(i);
        assertEq(c >= 0x80 && c <= 0x9F, false, "code unit " + c.toString(16) + ":");
    }
});

await test("malformed UTF-8 is dropped byte by byte, not passed through", () => {
    const s = "a" + ch(0xC0, 0xAF) + "b" + ch(0x80) + "c";
    assertEq(wire.safeText(s), "abc");
});

await test("truncation never leaves half a character", () => {
    const e = utf8("é");
    const out = wire.safeName(e.repeat(40), 7);   // 2 bytes each
    assertEq(out, "ééé...");
});

// peek_uint32 -----------------------------------------------------------

await test("peek_uint32 reads a length prefix without a reader", () => {
    const buf = wire.uint32(0x01020304) + "rest";
    assertEq(wire.peekUint32(buf), 0x01020304);
    // Lua: position 3 (1-based) -> 2 (0-based)
    assertEq(wire.peekUint32("xx" + wire.uint32(7), 2), 7);
});

await test("peek_uint32 reads the top of the range unsigned", () => {
    assertEq(wire.peekUint32("\xff\xff\xff\xff"), 0xFFFFFFFF);
});

await test("peek_uint32 refuses a short buffer with a wire error", () => {
    let ok = true, err = null;
    try { wire.peekUint32("abc"); } catch (e) { ok = false; err = e; }
    assertEq(ok, false);
    assertEq(String(err.message).indexOf("ssh.wire: truncated") >= 0, true, String(err));
    assertRaises(() => wire.peekUint32("abcd", 1), "past the end");
});

// gather ---------------------------------------------------------------

// A read function serving `pieces` in order, recording what it was asked for.
function reader(pieces) {
    const asked = [];
    let i = 0;
    const read = async (max) => {
        asked.push(max);
        const p = pieces[i++];
        if (Array.isArray(p)) return p;
        return [p];
    };
    return [read, asked];
}

await test("gather joins short reads until n bytes are held", async () => {
    const [read, asked] = reader(["ab", "c", "defg"]);
    const [buf, ok] = await wire.gather("x", 6, read);
    assertEq(ok, true);
    assertEq(buf, "xabcdefg");
    assertEq(asked[0], 5);      // asks only for what is missing
    assertEq(asked[1], 3);
});

await test("gather does not read when the buffer already holds n", async () => {
    const [read, asked] = reader([]);
    const [buf, ok] = await wire.gather("abcdef", 4, read);
    assertEq(ok, true);
    assertEq(buf, "abcdef");
    assertEq(asked.length, 0);
});

await test("gather reports end of stream and keeps what it had", async () => {
    const [read] = reader(["ab", ""]);
    const [buf, ok] = await wire.gather("", 5, read);
    assertEq(ok, false);
    assertEq(buf, "ab");
});

await test("gather passes a read failure through with its code", async () => {
    const [read] = reader(["a", [null, "reset", "io_error"]]);
    const [buf, ok, err, code] = await wire.gather("", 5, read);
    assertEq(ok, null);
    assertEq(buf, "a");
    assertEq(err, "reset");
    assertEq(code, "io_error");
});

await test("gather caps one read at max_chunk", async () => {
    const [read, asked] = reader(["abc", "def", "g"]);
    const [buf, ok] = await wire.gather("", 7, read, 3);
    assertEq(ok, true);
    assertEq(buf, "abcdefg");
    assertEq(asked[0], 3);
    assertEq(asked[1], 3);
    assertEq(asked[2], 1);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
