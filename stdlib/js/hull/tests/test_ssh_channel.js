// test_ssh_channel.js - Tests for hull:ssh:channel (twin of
// stdlib/lua/hull/tests/test_ssh_channel.lua).
//
// RFC 4254. The window cases are the point: the advertised window is a
// promise about how much Hull will hold for a peer.

import { channel } from "hull:ssh:channel";
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

const x = (n) => "x".repeat(n);

// a channel already opened against a peer
function opened(opts) {
    const c = channel.new(opts);
    c.handle(channel.parse(wire.writer()
        .byte(91).uint32(c.localId).uint32(7)
        .uint32((opts && opts.peerWindow) || 1000)
        .uint32((opts && opts.peerMax) || 100)
        .build()));
    return c;
}

// builders -------------------------------------------------------------------

await test("open advertises our window and packet cap", () => {
    const r = wire.reader(channel.buildOpen(3, 4096, 512));
    assertEq(r.byte(), channel.SSH_MSG_CHANNEL_OPEN);
    assertEq(r.string(), "session");
    assertEq(r.uint32(), 3);
    assertEq(r.uint32(), 4096);
    assertEq(r.uint32(), 512);
});

await test("exec sends the command as one opaque string", () => {
    const nasty = "echo 'a b'; rm -rf / & $(whoami) `id`";
    const r = wire.reader(channel.buildExec(7, nasty));
    assertEq(r.byte(), channel.SSH_MSG_CHANNEL_REQUEST);
    assertEq(r.uint32(), 7);
    assertEq(r.string(), "exec");
    assertEq(r.boolean(), true);
    assertEq(r.string(), nasty, "the command survives byte for byte");
});

await test("exec refuses a non-string command", () => {
    assertRaises(() => channel.buildExec(7, null), "nil command");
    assertRaises(() => channel.buildExec(7, ["ls"]), "table command");
});

await test("subsystem request is built for sftp", () => {
    const r = wire.reader(channel.buildSubsystem(7, "sftp"));
    r.byte(); r.uint32();
    assertEq(r.string(), "subsystem");
    r.boolean();
    assertEq(r.string(), "sftp");
});

await test("data, eof, close and adjust all name the recipient", () => {
    for (const b of [
        channel.buildData(7, "x"), channel.buildEof(7),
        channel.buildClose(7), channel.buildWindowAdjust(7, 10),
    ]) {
        const r = wire.reader(b);
        r.byte();
        assertEq(r.uint32(), 7);
    }
});

// parsing --------------------------------------------------------------------

await test("open confirmation carries both ids and the peer limits", () => {
    const p = wire.writer().byte(91).uint32(0).uint32(7).uint32(2048).uint32(256).build();
    const m = channel.parse(p);
    assertEq(m.type, "open_confirmation");
    assertEq(m.recipient, 0);
    assertEq(m.sender, 7);
    assertEq(m.window, 2048);
    assertEq(m.maxPacket, 256);
});

await test("ssh_audit44.duplicate_open_confirmation_refused", () => {
    // A second confirmation would replace the peer's channel id and reset the
    // send window and packet size mid-session: a protocol error.
    const ch = channel.new({ id: 0 });
    const conf = (sender, window) => channel.parse(wire.writer().byte(91).uint32(0)
        .uint32(sender).uint32(window).uint32(256).build());
    ch.handle(conf(7, 2048));
    assertEq(ch.remoteId, 7);
    assertRaises(() => ch.handle(conf(9, 4096)), "a second confirmation");
    assertEq(ch.remoteId, 7, "the peer id stands:");
    assertEq(ch.sendWindow, 2048, "the window stands:");
    // Nor does a confirmation follow a failure.
    const ch2 = channel.new({ id: 0 });
    ch2.handle(channel.parse(wire.writer().byte(92).uint32(0).uint32(4)
        .string("no").string("en").build()));
    assertRaises(() => ch2.handle(conf(7, 2048)), "a confirmation after a failure");
});

await test("open failure carries a reason a human can read", () => {
    const p = wire.writer().byte(92).uint32(0).uint32(4)
        .string("administratively prohibited").string("en").build();
    const m = channel.parse(p);
    assertEq(m.type, "open_failure");
    assertEq(m.reason, 4);
    assertEq(m.description, "administratively prohibited");
});

await test("stdout and stderr are distinguishable", () => {
    const out = channel.parse(wire.writer().byte(94).uint32(0).string("to stdout").build());
    assertEq(out.type, "data");
    assertEq(out.data, "to stdout");

    const err = channel.parse(wire.writer().byte(95).uint32(0).uint32(1)
        .string("to stderr").build());
    assertEq(err.type, "extended_data");
    assertEq(err.stderr, true);
    assertEq(err.data, "to stderr");
});

await test("an unknown extended data code is not treated as stderr", () => {
    const m = channel.parse(wire.writer().byte(95).uint32(0).uint32(99).string("?").build());
    assertEq(m.stderr, false);
    assertEq(m.code, 99);
});

await test("exit-status is parsed", () => {
    const p = wire.writer().byte(98).uint32(0).string("exit-status")
        .boolean(false).uint32(42).build();
    const m = channel.parse(p);
    assertEq(m.request, "exit-status");
    assertEq(m.exitStatus, 42);
});

await test("exit-signal is parsed", () => {
    const p = wire.writer().byte(98).uint32(0).string("exit-signal")
        .boolean(false).string("TERM").boolean(false)
        .string("terminated").string("en").build();
    const m = channel.parse(p);
    assertEq(m.signal, "TERM");
    assertEq(m.coreDumped, false);
});

await test("an unknown channel request is returned, not refused", () => {
    const p = wire.writer().byte(98).uint32(0).string("keepalive@openssh.com")
        .boolean(true).build();
    const m = channel.parse(p);
    assertEq(m.request, "keepalive@openssh.com");
});

await test("an unexpected message number raises", () => {
    assertRaises(() => channel.parse(wire.writer().byte(200).uint32(0).build()), "message 200");
});

// channel identity -----------------------------------------------------------

await test("a message for another channel is refused", () => {
    const c = opened({ id: 3 });
    assertRaises(() => c.handle({ type: "data", recipient: 9, data: "x" }), "wrong channel");
});

await test("open confirmation records the peer id and limits", () => {
    const c = opened({ id: 3, peerWindow: 500, peerMax: 50 });
    assertEq(c.open, true);
    assertEq(c.remoteId, 7);
    assertEq(c.sendWindow, 500);
    assertEq(c.sendMaxPacket, 50);
});

await test("open failure leaves the channel closed", () => {
    const c = channel.new({ id: 1 });
    c.handle({ type: "open_failure", recipient: 1, reason: 1, description: "no", language: "" });
    assertEq(c.open, false);
    assertEq(c.closed, true);
});

// receive window -------------------------------------------------------------

await test("received data draws down the advertised window", () => {
    const c = opened({ id: 0, window: 100, maxPacket: 100 });
    c.handle({ type: "data", recipient: 0, data: x(30) });
    assertEq(c.recvWindow, 70);
});

await test("data beyond the advertised window is refused", () => {
    const c = opened({ id: 0, window: 50, maxPacket: 1000 });
    assertRaises(() => c.handle({ type: "data", recipient: 0, data: x(51) }), "over window");
});

await test("data over the packet cap is refused", () => {
    const c = opened({ id: 0, window: 10000, maxPacket: 64 });
    assertRaises(() => c.handle({ type: "data", recipient: 0, data: x(65) }), "over packet cap");
});

await test("stderr draws on the same window as stdout", () => {
    const c = opened({ id: 0, window: 100, maxPacket: 100 });
    c.handle({ type: "data", recipient: 0, data: x(30) });
    c.handle({ type: "extended_data", recipient: 0, code: 1, data: "y".repeat(30), stderr: true });
    assertEq(c.recvWindow, 40);
});

await test("the window is topped up only once it is half spent", () => {
    const c = opened({ id: 0, window: 100, maxPacket: 100 });
    c.handle({ type: "data", recipient: 0, data: x(40) });
    assertEq(c.windowAdjustment(), null, "not due yet");

    c.handle({ type: "data", recipient: 0, data: x(20) });
    const adj = c.windowAdjustment();
    assertEq(adj !== null, true, "due after half is spent");
    const r = wire.reader(adj);
    assertEq(r.byte(), channel.SSH_MSG_CHANNEL_WINDOW_ADJUST);
    assertEq(r.uint32(), 7);
    assertEq(r.uint32(), 60);
    assertEq(c.recvWindow, 100, "restored to the initial size");
});

// send window ----------------------------------------------------------------

await test("nothing can be sent before the channel opens", () => {
    const c = channel.new({ id: 0 });
    assertEq(c.canSend(1), false);
    assertEq(c.sendable(), 0);
});

await test("sending draws down the granted window", () => {
    const c = opened({ id: 0, peerWindow: 100, peerMax: 100 });
    c.dataMessage(x(40));
    assertEq(c.sendWindow, 60);
});

await test("sending more than the window is refused, not truncated", () => {
    const c = opened({ id: 0, peerWindow: 10, peerMax: 100 });
    assertRaises(() => c.dataMessage(x(11)), "over send window");
});

await test("sending more than the peer packet cap is refused", () => {
    const c = opened({ id: 0, peerWindow: 10000, peerMax: 64 });
    assertRaises(() => c.dataMessage(x(65)), "over peer packet cap");
});

await test("sendable is the smaller of window and packet cap", () => {
    assertEq(opened({ peerWindow: 500, peerMax: 100 }).sendable(), 100);
    assertEq(opened({ peerWindow: 50, peerMax: 100 }).sendable(), 50);
});

await test("a window adjust from the peer lets more through", () => {
    const c = opened({ id: 0, peerWindow: 10, peerMax: 100 });
    c.dataMessage(x(10));
    assertEq(c.canSend(1), false);
    c.handle({ type: "window_adjust", recipient: 0, add: 90 });
    assertEq(c.sendWindow, 90);
    assertEq(c.canSend(90), true);
});

await test("a window adjust that would overflow is refused", () => {
    const c = opened({ id: 0, peerWindow: 10, peerMax: 100 });
    assertRaises(() => c.handle({ type: "window_adjust", recipient: 0, add: 0xFFFFFFFF }), "overflow");
});

// results --------------------------------------------------------------------

await test("exit status is reported", () => {
    const c = opened({ id: 0 });
    c.handle(channel.parse(wire.writer().byte(98).uint32(0)
        .string("exit-status").boolean(false).uint32(3).build()));
    assertEq(c.result().status, 3);
});

await test("a signalled command has no exit status", () => {
    const c = opened({ id: 0 });
    c.handle(channel.parse(wire.writer().byte(98).uint32(0)
        .string("exit-signal").boolean(false).string("KILL")
        .boolean(true).string("killed").string("").build()));
    const res = c.result();
    assertEq(res.status, null);
    assertEq(res.signal.signal, "KILL");
    assertEq(res.signal.coreDumped, true);
});

await test("eof and close are tracked separately", () => {
    const c = opened({ id: 0 });
    c.handle({ type: "eof", recipient: 0 });
    assertEq(c.result().eof, true);
    assertEq(c.result().closed, false);
    c.handle({ type: "close", recipient: 0 });
    assertEq(c.result().closed, true);
});

await test("a closed channel sends nothing further", () => {
    const c = opened({ id: 0, peerWindow: 100, peerMax: 100 });
    c.handle({ type: "close", recipient: 0 });
    assertEq(c.canSend(1), false);
    assertEq(c.windowAdjustment(), null);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
