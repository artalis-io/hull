// test_ssh_packet.js - Tests for hull:ssh:packet (twin of test_ssh_packet.lua)
//
// Version exchange (RFC 4253 section 4.2) and the binary packet protocol
// (section 6). The length-field cases are the point.

import { packet } from "hull:ssh:packet";
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

// deterministic padding so a framed packet is reproducible
const zeros = (n) => "\x00".repeat(n);

// identification string ------------------------------------------------

await test("build_ident produces an RFC 4253 line", () => {
    assertEq(packet.buildIdent("Hull_0.15"), "SSH-2.0-Hull_0.15\r\n");
});

await test("build_ident refuses characters that break the framing", () => {
    assertRaises(() => packet.buildIdent("Hull 0.15"), "space");
    assertRaises(() => packet.buildIdent("Hull-0.15"), "minus");
    assertRaises(() => packet.buildIdent("Hull\r\n"), "crlf");
    assertRaises(() => packet.buildIdent(""), "empty");
});

await test("parse_ident accepts a plain line", () => {
    const [id] = packet.parseIdent("SSH-2.0-OpenSSH_9.6");
    assertEq(id.protoversion, "2.0");
    assertEq(id.software, "OpenSSH_9.6");
    assertEq(id.comments, null);
});

await test("parse_ident splits comments on the first space", () => {
    const [id] = packet.parseIdent("SSH-2.0-OpenSSH_9.6 Ubuntu-3");
    assertEq(id.software, "OpenSSH_9.6");
    assertEq(id.comments, "Ubuntu-3");
});

await test("parse_ident rejects a non-identification line", () => {
    const [id, err] = packet.parseIdent("hello there");
    assertEq(id, null);
    assertEq(err, "not an identification string");
});

await test("parse_ident rejects protocol versions we do not speak", () => {
    assertEq(packet.parseIdent("SSH-1.99-OldServer")[0], null);
    assertEq(packet.parseIdent("SSH-1.5-Ancient")[0], null);
});

await test("parse_ident rejects an over-long line", () => {
    assertEq(packet.parseIdent("SSH-2.0-" + "x".repeat(300))[0], null);
});

// padding ---------------------------------------------------------------

await test("padding aligns the whole packet to the block size", () => {
    for (const block of [8, 16]) {
        for (let payloadLen = 0; payloadLen <= 40; payloadLen++) {
            const pad = packet.paddingFor(payloadLen, block, true);
            assertEq(pad >= 4, true, "padding at least 4 for " + payloadLen);
            assertEq((4 + 1 + payloadLen + pad) % block, 0,
                "aligned for len " + payloadLen + " block " + block);
        }
    }
});

await test("padding excludes the length field for an AEAD", () => {
    for (let payloadLen = 0; payloadLen <= 40; payloadLen++) {
        const pad = packet.paddingFor(payloadLen, 16, false);
        assertEq(pad >= 4, true);
        assertEq((1 + payloadLen + pad) % 16, 0);
    }
});

await test("a block smaller than 8 is raised to 8", () => {
    assertEq(packet.paddingFor(0, 1, true), packet.paddingFor(0, 8, true));
});

// framing ----------------------------------------------------------------

await test("frame and parse round trip", () => {
    for (const payload of ["", "x", "abc".repeat(100)]) {
        const b = packet.frame(payload, 8, zeros);
        const [got, used] = packet.parse(b, 8);
        assertEq(got, payload);
        assertEq(used, b.length);
    }
});

await test("a framed packet has the declared shape", () => {
    const b = packet.frame("hi", 8, zeros);
    const packetLength = wire.peekUint32(b, 0);
    const paddingLength = b.charCodeAt(4);
    assertEq(b.length, packetLength + 4);
    assertEq(packetLength, 2 + paddingLength + 1);
    assertEq(b.length % 8, 0);
});

await test("frame demands real randomness from its caller", () => {
    assertRaises(() => packet.frame("x", 8, null), "no rng");
    assertRaises(() => packet.frame("x", 8, () => "short"), "wrong length from rng");
});

await test("parse asks for more when the packet is incomplete", () => {
    const b = packet.frame("hello world", 8, zeros);
    for (let n = 0; n <= b.length - 1; n++) {
        const [got, err] = packet.parse(b.slice(0, n), 8);
        assertEq(got, null, "partial of " + n + " should not parse");
        assertEq(err, "need_more");
    }
    assertEq(packet.parse(b, 8)[0], "hello world");
});

await test("parse leaves trailing bytes alone", () => {
    const b = packet.frame("one", 8, zeros) + packet.frame("two", 8, zeros);
    const [first, used] = packet.parse(b, 8);
    assertEq(first, "one");
    assertEq(packet.parse(b.slice(used), 8)[0], "two");
});

// hostile length fields ---------------------------------------------------

await test("an enormous declared length is refused immediately", () => {
    const b = "\xff\xff\xff\xff" + "\x00".repeat(32);
    assertRaises(() => packet.parse(b, 8), "huge length");
});

await test("a length below the minimum is refused", () => {
    const b = wire.uint32(3) + "\x00".repeat(32);
    assertRaises(() => packet.parse(b, 8), "tiny length");
});

await test("a misaligned length is refused", () => {
    const b = wire.uint32(13) + "\x00".repeat(32);
    assertRaises(() => packet.parse(b, 8), "misaligned");
});

await test("padding below the minimum is refused", () => {
    const body = String.fromCharCode(3) + "\x00".repeat(11);   // padding_length = 3
    const b = wire.uint32(body.length) + body;
    assertRaises(() => packet.parse(b, 8), "short padding");
});

await test("padding larger than the packet is refused", () => {
    const body = String.fromCharCode(200) + "\x00".repeat(11);
    const b = wire.uint32(body.length) + body;
    assertRaises(() => packet.parse(b, 8), "padding exceeds packet");
});

await test("the maximum packet size is enforced on frame too", () => {
    assertRaises(() => packet.frame("x".repeat(packet.MAX_PACKET), 8, zeros), "oversize frame");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
