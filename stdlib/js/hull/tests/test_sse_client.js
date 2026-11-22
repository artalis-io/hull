// test_sse_client.js - Tests for hull:sse-client's event-stream parser
//
// Run via the C test harness (test_js.c, js_stdlib.sse_client_suite).

import { sseClient } from "hull:sse-client";

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

function eq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function run(chunks) {
    const p = sseClient.parser();
    const out = [];
    for (const c of chunks) for (const ev of p.feed(c)) out.push(ev);
    for (const ev of p.finish()) out.push(ev);
    return out;
}

test("one event with a name, an id and data", () => {
    const evs = run(["event: tick\nid: 7\ndata: hello\n\n"]);
    eq(evs.length, 1);
    eq(evs[0].event, "tick"); eq(evs[0].id, "7"); eq(evs[0].data, "hello");
});

test("the default event name is message", () => {
    const evs = run(["data: x\n\n"]);
    eq(evs[0].event, "message"); eq(evs[0].id, null);
});

test("data lines join with LF", () => {
    eq(run(["data: a\ndata: b\ndata:c\n\n"])[0].data, "a\nb\nc");
});

test("CRLF and CR end lines too", () => {
    const evs = run(["data: a\r\n\r\ndata: b\r\rdata: c\n\n"]);
    eq(evs.length, 3); eq(evs[0].data, "a"); eq(evs[1].data, "b"); eq(evs[2].data, "c");
});

test("a CR at the end of a chunk waits for the next", () => {
    const evs = run(["data: a\r", "\n\r", "\n"]);
    eq(evs.length, 1); eq(evs[0].data, "a");
});

test("any split of the input gives the same events", () => {
    const text = "event: e\r\nid: 1\r\ndata: x\r\ndata: y\r\n\r\n: note\ndata: z\n\n";
    const whole = run([text]);
    const split = run(text.split(""));
    eq(split.length, whole.length);
    for (let i = 0; i < whole.length; i++) {
        eq(split[i].event, whole[i].event); eq(split[i].data, whole[i].data);
        eq(split[i].id, whole[i].id);
    }
});

test("a leading byte-order mark is skipped", () => {
    eq(run(["﻿data: x\n\n"])[0].data, "x");
});

test("comment lines are ignored", () => {
    const evs = run([": keep-alive\n\n: x\ndata: y\n\n"]);
    eq(evs.length, 1); eq(evs[0].data, "y");
});

test("a blank line with no data dispatches nothing", () => {
    eq(run(["event: x\n\n\n"]).length, 0);
});

test("an empty data field dispatches an empty event", () => {
    const evs = run(["data\n\n"]);
    eq(evs.length, 1); eq(evs[0].data, "");
});

test("only one leading space is removed from a value", () => {
    eq(run(["data:  two\n\n"])[0].data, " two");
});

test("the last id persists across events; an id with NUL is ignored", () => {
    const evs = run(["id: 1\ndata: a\n\ndata: b\n\nid: x\0y\ndata: c\n\n"]);
    eq(evs[0].id, "1"); eq(evs[1].id, "1"); eq(evs[2].id, "1");
});

test("retry takes digits only", () => {
    const evs = run(["retry: 1500\ndata: a\n\nretry: 1.5\ndata: b\n\n"]);
    eq(evs[0].retry, 1500); eq(evs[1].retry, null);
});

test("unknown fields are ignored", () => {
    const evs = run(["foo: bar\ndata: a\n\n"]);
    eq(evs.length, 1); eq(evs[0].data, "a");
});

test("an event the input ended inside is dropped", () => {
    eq(run(["data: a\n\ndata: b\n"]).length, 1);
    eq(run(["data: a"]).length, 0);
});

test("a trailing CR at the end of the input completes its line", () => {
    const evs = run(["data: a\r\r"]);
    eq(evs.length, 1); eq(evs[0].data, "a");
});

test("feed refuses a non-string", () => {
    let threw = false;
    try { sseClient.parser().feed(42); } catch (e) { threw = e instanceof TypeError; }
    eq(threw, true);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
