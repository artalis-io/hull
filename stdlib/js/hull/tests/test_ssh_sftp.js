// test_ssh_sftp.js - Tests for hull:ssh:sftp, the codec (twin of
// stdlib/lua/hull/tests/test_ssh_sftp.lua).

import { sftp } from "hull:ssh:sftp";
import { wire } from "hull:ssh:wire";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try { await fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + ((e && e.stack) || e)); }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}

function assertRaises(fn, msg) {
    let ok = true;
    try { fn(); } catch (e) { ok = false; }
    if (ok) throw new Error((msg || "should have raised") + " but did not");
}

// framing --------------------------------------------------------------------

await test("frame and parse_frame round trip", () => {
    const p = sftp.buildInit();
    const framed = sftp.frame(p);
    const [got, used] = sftp.parseFrame(framed);
    assertEq(got, p);
    assertEq(used, framed.length);
});

await test("parse_frame asks for more while incomplete", () => {
    const framed = sftp.frame(sftp.buildStat(1, "/etc/hostname"));
    for (let n = 0; n <= framed.length - 1; n++) {
        const [got, err] = sftp.parseFrame(framed.slice(0, n));
        assertEq(got, null, "partial " + n);
        assertEq(err, "need_more");
    }
});

await test("parse_frame leaves trailing packets alone", () => {
    const pa = sftp.buildStat(1, "/a"), pb = sftp.buildStat(2, "/b");
    const buf = sftp.frame(pa) + sftp.frame(pb);
    const [first, used] = sftp.parseFrame(buf);
    assertEq(first, pa);
    assertEq(used, sftp.frame(pa).length);
    const [second] = sftp.parseFrame(buf.slice(used));
    assertEq(second, pb);
});

await test("an impossible declared length is refused immediately", () => {
    assertRaises(() => sftp.parseFrame("\xff\xff\xff\xff" + "short"), "huge length");
    assertRaises(() => sftp.parseFrame("\x00\x00\x00\x00"), "zero length");
});

await test("frame refuses to send an oversized packet", () => {
    assertRaises(() => sftp.frame("x".repeat(sftp.MAX_PACKET + 1)), "oversize");
});

// requests -------------------------------------------------------------------

await test("init declares version 3", () => {
    const r = wire.reader(sftp.buildInit());
    assertEq(r.byte(), sftp.FXP_INIT);
    assertEq(r.uint32(), 3);
});

await test("a path with shell metacharacters travels intact", () => {
    const nasty = "/tmp/; rm -rf / && echo $(whoami) `id` 'quoted' \"double\"";
    const r = wire.reader(sftp.buildOpen(1, nasty, sftp.FXF_READ));
    assertEq(r.byte(), sftp.FXP_OPEN);
    assertEq(r.uint32(), 1);
    assertEq(r.string(), nasty, "byte for byte");
});

await test("a path with a newline or NUL travels intact too", () => {
    const weird = "/tmp/two\nlines\x00and-a-nul";
    const r = wire.reader(sftp.buildStat(5, weird));
    r.byte(); r.uint32();
    assertEq(r.string(), weird);
});

await test("open carries flags and an empty attribute block", () => {
    const r = wire.reader(sftp.buildOpen(9, "/f",
        sftp.FXF_WRITE | sftp.FXF_CREAT | sftp.FXF_TRUNC));
    r.byte(); r.uint32(); r.string();
    assertEq(r.uint32(), 0x02 | 0x08 | 0x10);
    assertEq(r.uint32(), 0, "empty attrs: let the server pick the mode");
});

await test("read names an offset and a length", () => {
    const r = wire.reader(sftp.buildRead(2, "H", 4294967296, 32768));
    assertEq(r.byte(), sftp.FXP_READ);
    assertEq(r.uint32(), 2);
    assertEq(r.string(), "H");
    assertEq(r.uint64(), 4294967296, "offsets are 64-bit");
    assertEq(r.uint32(), 32768);
});

await test("write carries data at an offset", () => {
    const r = wire.reader(sftp.buildWrite(3, "H", 100, "payload"));
    r.byte(); r.uint32();
    assertEq(r.string(), "H");
    assertEq(r.uint64(), 100);
    assertEq(r.string(), "payload");
});

await test("rename carries both paths", () => {
    const r = wire.reader(sftp.buildRename(4, "/from", "/to"));
    r.byte(); r.uint32();
    assertEq(r.string(), "/from");
    assertEq(r.string(), "/to");
});

await test("mkdir can set a mode", () => {
    const r = wire.reader(sftp.buildMkdir(5, "/d", { permissions: 0x1ED }));
    r.byte(); r.uint32(); r.string();
    assertEq(r.uint32(), sftp.ATTR_PERMISSIONS);
    assertEq(r.uint32(), 0x1ED);
});

// responses ------------------------------------------------------------------

await test("version response is parsed", () => {
    const p = wire.writer().byte(sftp.FXP_VERSION).uint32(3).build();
    const m = sftp.parse(p);
    assertEq(m.type, "version");
    assertEq(m.version, 3);
});

await test("status ok and eof are distinguished", () => {
    const ok = sftp.parse(wire.writer().byte(101).uint32(1).uint32(0)
        .string("").string("").build());
    assertEq(ok.ok, true);
    assertEq(ok.eof, false);

    const eof = sftp.parse(wire.writer().byte(101).uint32(1).uint32(1)
        .string("").string("").build());
    assertEq(eof.ok, false);
    assertEq(eof.eof, true);
});

await test("status falls back to a readable text", () => {
    const m = sftp.parse(wire.writer().byte(101).uint32(1).uint32(3)
        .string("").string("").build());
    assertEq(m.text, "permission denied");
});

await test("a server message is preferred over our text", () => {
    const m = sftp.parse(wire.writer().byte(101).uint32(1).uint32(4)
        .string("disk full").string("en").build());
    assertEq(m.text, "disk full");
});

await test("a server message reaches the caller without its controls", () => {
    const raw = "denied\x1b[2K\rOK" + String.fromCharCode(10) + "forged line";
    const m = sftp.parse(wire.writer().byte(101).uint32(1).uint32(3)
        .string(raw).string("en").build());
    assertEq(m.text, "denied[2KOKforged line");
    assertEq(m.message, raw);
});

await test("a status with no message still has text", () => {
    const m = sftp.parse(wire.writer().byte(101).uint32(1).uint32(2).build());
    assertEq(m.text, "no such file");
});

await test("handle and data responses are parsed", () => {
    const h = sftp.parse(wire.writer().byte(102).uint32(1).string("HANDLE").build());
    assertEq(h.handle, "HANDLE");
    const d = sftp.parse(wire.writer().byte(103).uint32(1).string("bytes").build());
    assertEq(d.data, "bytes");
});

await test("a name reply is parsed with attributes", () => {
    const p = wire.writer().byte(104).uint32(1).uint32(2)
        .string("a.txt").string("-rw-r--r-- ...")
            .uint32(sftp.ATTR_SIZE).uint64(1234)
        .string("sub").string("drwxr-xr-x ...")
            .uint32(sftp.ATTR_PERMISSIONS).uint32(0x41ED)
        .build();
    const m = sftp.parse(p);
    assertEq(m.type, "name");
    assertEq(m.names.length, 2);
    assertEq(m.names[0].filename, "a.txt");
    assertEq(m.names[0].attrs.size, 1234);
    assertEq(m.names[1].attrs.isDir, true);
});

await test("attrs decode only the flagged fields", () => {
    const blob = sftp.encodeAttrs({ size: 42, permissions: 0x1A4 });
    const a = sftp.decodeAttrs(wire.reader(blob));
    assertEq(a.size, 42);
    assertEq(a.permissions, 0x1A4);
    assertEq(a.uid, undefined);
    assertEq(a.mtime, undefined);
});

await test("a dishonest extended count runs out of buffer", () => {
    const blob = wire.writer().uint32(sftp.ATTR_EXTENDED).uint32(1000).build();
    assertRaises(() => sftp.decodeAttrs(wire.reader(blob)), "lying extended count");
});

await test("an unexpected response type raises", () => {
    assertRaises(() => sftp.parse(wire.writer().byte(200).uint32(1).build()), "type 200");
});

// names from the server ------------------------------------------------------

await test("an ordinary name is usable", () => {
    assertEq(sftp.unsafeName("report.txt"), null);
    assertEq(sftp.unsafeName("with spaces and-dashes.tar.gz"), null);
});

await test("traversal names are refused", () => {
    assertEq(sftp.unsafeName("..") !== null, true);
    assertEq(sftp.unsafeName(".") !== null, true);
    assertEq(sftp.unsafeName("../../.ssh/authorized_keys") !== null, true);
    assertEq(sftp.unsafeName("/etc/passwd") !== null, true);
    assertEq(sftp.unsafeName("sub/file") !== null, true);
});

await test("a backslash is refused too", () => {
    assertEq(sftp.unsafeName("..\\..\\evil") !== null, true);
});

await test("NUL and control characters are refused", () => {
    assertEq(sftp.unsafeName("ok\x00/etc/passwd") !== null, true);
    assertEq(sftp.unsafeName("clear\x1b[2Jscreen") !== null, true);
    assertEq(sftp.unsafeName("") !== null, true);
});

await test("safe_names reports what it refused rather than dropping it", () => {
    const [ok, refused] = sftp.safeNames([
        { filename: "good.txt" },
        { filename: "../escape" },
        { filename: "also-good" },
        { filename: "bad\x00name" },
    ]);
    assertEq(ok.length, 2);
    assertEq(ok[0].filename, "good.txt");
    assertEq(refused.length, 2);
    assertEq(refused[0].filename, "../escape");
    assertEq(typeof refused[0].reason, "string");
});

await test("unsafe_name does not try to sanitize", () => {
    const r = sftp.unsafeName("../x");
    assertEq(typeof r, "string");
    assertEq(r !== "../x", true);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
