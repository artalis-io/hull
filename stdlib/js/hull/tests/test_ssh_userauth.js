// test_ssh_userauth.js - Tests for hull:ssh:userauth (twin of
// stdlib/lua/hull/tests/test_ssh_userauth.lua).
//
// RFC 4252. The signed-blob cases are the point: the session identifier binds
// a signature to one connection, and the user name and service bind it to one
// identity.

import { userauth } from "hull:ssh:userauth";
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

const KEY_BLOB = wire.writer().string("ssh-ed25519").string("\x01".repeat(32)).build();
const SIG = "\x09".repeat(64);

// service request -----------------------------------------------------------

await test("service request names ssh-userauth", () => {
    const r = wire.reader(userauth.buildServiceRequest());
    assertEq(r.byte(), userauth.SSH_MSG_SERVICE_REQUEST);
    assertEq(r.string(), "ssh-userauth");
});

await test("service accept round trips", () => {
    const p = wire.writer().byte(6).string("ssh-userauth").build();
    assertEq(userauth.parseServiceAccept(p), "ssh-userauth");
});

await test("service accept rejects another message", () => {
    const p = wire.writer().byte(7).string("ssh-userauth").build();
    assertRaises(() => userauth.parseServiceAccept(p), "wrong type");
});

// the signed blob ------------------------------------------------------------

await test("signed blob is the RFC 4252 section 7 order", () => {
    const blob = userauth.signedBlob("SESSION", "operator", KEY_BLOB);
    const r = wire.reader(blob);
    assertEq(r.string(), "SESSION");
    assertEq(r.byte(), userauth.SSH_MSG_USERAUTH_REQUEST);
    assertEq(r.string(), "operator");
    assertEq(r.string(), "ssh-connection");
    assertEq(r.string(), "publickey");
    assertEq(r.boolean(), true);
    assertEq(r.string(), "ssh-ed25519");
    assertEq(r.string(), KEY_BLOB);
    assertEq(r.remaining(), 0);
});

await test("the session identifier comes first", () => {
    const blob = userauth.signedBlob("SESSION", "operator", KEY_BLOB);
    assertEq(wire.reader(blob).string(), "SESSION");
});

await test("a different session gives a different blob to sign", () => {
    const a = userauth.signedBlob("SESSION-A", "operator", KEY_BLOB);
    const b = userauth.signedBlob("SESSION-B", "operator", KEY_BLOB);
    assertEq(a !== b, true);
});

await test("a different user gives a different blob to sign", () => {
    const a = userauth.signedBlob("SESSION", "operator", KEY_BLOB);
    const b = userauth.signedBlob("SESSION", "root", KEY_BLOB);
    assertEq(a !== b, true);
});

await test("a different key gives a different blob to sign", () => {
    const other = wire.writer().string("ssh-ed25519").string("\x02".repeat(32)).build();
    const a = userauth.signedBlob("SESSION", "operator", KEY_BLOB);
    const b = userauth.signedBlob("SESSION", "operator", other);
    assertEq(a !== b, true);
});

await test("signed blob refuses a missing session or user", () => {
    assertRaises(() => userauth.signedBlob(null, "operator", KEY_BLOB), "no session");
    assertRaises(() => userauth.signedBlob("", "operator", KEY_BLOB), "empty session");
    assertRaises(() => userauth.signedBlob("SESSION", "", KEY_BLOB), "empty user");
});

await test("the signed boolean is TRUE, not the query form", () => {
    const r = wire.reader(userauth.signedBlob("S", "u", KEY_BLOB));
    r.string(); r.byte(); r.string(); r.string(); r.string();
    assertEq(r.boolean(), true);
});

// the request ----------------------------------------------------------------

await test("request mirrors the signed blob field for field", () => {
    const sigBlob = userauth.signatureBlob(SIG);
    const req = userauth.buildRequest("operator", KEY_BLOB, sigBlob);
    const signed = userauth.signedBlob("SESSION", "operator", KEY_BLOB);

    const rq = wire.reader(req);
    const sg = wire.reader(signed);
    assertEq(sg.string(), "SESSION");

    assertEq(rq.byte(), sg.byte());
    assertEq(rq.string(), sg.string());
    assertEq(rq.string(), sg.string());
    assertEq(rq.string(), sg.string());
    assertEq(rq.boolean(), sg.boolean());
    assertEq(rq.string(), sg.string());
    assertEq(rq.string(), sg.string());
    assertEq(rq.string(), sigBlob);
    assertEq(rq.remaining(), 0);
    assertEq(sg.remaining(), 0);
});

await test("signature blob wraps the raw signature", () => {
    const b = userauth.signatureBlob(SIG);
    const r = wire.reader(b);
    assertEq(r.string(), "ssh-ed25519");
    assertEq(r.string(), SIG);
});

await test("signature blob refuses the wrong length", () => {
    assertRaises(() => userauth.signatureBlob("\x09".repeat(63)), "63 bytes");
    assertRaises(() => userauth.signatureBlob("\x09".repeat(65)), "65 bytes");
});

// responses ------------------------------------------------------------------

await test("success is recognised", () => {
    const p = wire.writer().byte(52).build();
    assertEq(userauth.parseResponse(p).type, "success");
});

await test("failure carries the methods that can continue", () => {
    const p = wire.writer().byte(51).namelist(["publickey", "password"]).boolean(false).build();
    const got = userauth.parseResponse(p);
    assertEq(got.type, "failure");
    assertEq(got.methods.length, 2);
    assertEq(got.partial, false);
});

await test("partial success is a failure, not a success", () => {
    const p = wire.writer().byte(51).namelist(["publickey"]).boolean(true).build();
    const got = userauth.parseResponse(p);
    assertEq(got.type, "failure");
    assertEq(got.partial, true);
});

await test("can_retry_publickey reads the continue list", () => {
    const again = userauth.parseResponse(
        wire.writer().byte(51).namelist(["publickey"]).boolean(false).build());
    assertEq(userauth.canRetryPublickey(again), true);

    const done = userauth.parseResponse(
        wire.writer().byte(51).namelist(["password"]).boolean(false).build());
    assertEq(userauth.canRetryPublickey(done), false);

    const ok = userauth.parseResponse(wire.writer().byte(52).build());
    assertEq(userauth.canRetryPublickey(ok), false);
});

await test("an unexpected message is a protocol error", () => {
    const p = wire.writer().byte(99).build();
    assertRaises(() => userauth.parseResponse(p), "message 99");
});

await test("a truncated failure is refused", () => {
    const p = wire.writer().byte(51).build();
    assertRaises(() => userauth.parseResponse(p), "truncated");
});

// banners --------------------------------------------------------------------

await test("banner text is returned", () => {
    const p = wire.writer().byte(53).string("Authorised use only").string("en").build();
    const got = userauth.parseResponse(p);
    assertEq(got.type, "banner");
    assertEq(got.message, "Authorised use only");
    assertEq(got.language, "en");
});

await test("banner escapes are stripped", () => {
    const nasty = "ok\x1b[2J\x1b[Hall clear\x07";
    const p = wire.writer().byte(53).string(nasty).string("").build();
    const got = userauth.parseResponse(p);
    assertEq(got.message.indexOf("\x1b"), -1, "escape survived");
    assertEq(got.message.indexOf("\x07"), -1, "bell survived");
    assertEq(got.message, "ok[2J[Hall clear");
    assertEq(got.rawMessage, nasty);
});

await test("banner keeps newline and tab", () => {
    const p = wire.writer().byte(53).string("line one\nline\ttwo").string("").build();
    assertEq(userauth.parseResponse(p).message, "line one\nline\ttwo");
});

await test("a banner loses NUL and DEL", () => {
    const p = wire.writer().byte(53).string("a\x00b\x7fc").string("").build();
    assertEq(userauth.parseResponse(p).message, "abc");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
