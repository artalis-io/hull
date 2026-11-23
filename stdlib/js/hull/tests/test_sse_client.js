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
    eq(evs[0].retry, 1500); eq(evs[1].retry, 1500);
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

// ── audit 52 ──────────────────────────────────────────────────────────

async function atest(name, fn) {
    try {
        await fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + (e && e.message));
    }
}

function codeOf(fn) {
    try { fn(); } catch (e) { return e.code; }
    return undefined;
}

// A fake stream: `chunks` read in order, then `final` (null = the end, or
// an Error to throw); one read at a time (a second is refused "busy", as
// the binding refuses it); counts close() calls.
function fake(chunks, final) {
    let i = 0;
    let busy = false;
    const r = { closes: 0 };
    r.readText = () => {
        if (busy) {
            const e = new Error("stream.readText(): a read is already waiting");
            e.code = "busy";
            throw e;
        }
        busy = true;
        return Promise.resolve().then(() => {
            busy = false;
            if (i < chunks.length) return chunks[i++];
            if (final) throw final;
            return null;
        });
    };
    r.close = () => { r.closes++; };
    return r;
}

test("sse_client_audit52.long_line_linear", () => {
    // A 1 MB line in 64-byte chunks: each chunk is scanned once. The old
    // parser rescanned the whole pending line per chunk.
    const p = sseClient.parser();
    const piece = "x".repeat(64);
    const t0 = Date.now();
    p.feed("data: ");
    for (let i = 1; i <= 16000; i++) {
        p.feed(piece);
        if (i % 256 === 0 && Date.now() - t0 > 3000)
            throw new Error("quadratic: " + i + " chunks took over 3 s");
    }
    const evs = p.feed("\n\n");
    eq(evs.length, 1); eq(evs[0].data.length, 16000 * 64);
});

test("sse_client_audit52.tiny_chunks", () => {
    // 200 KB of a line fed one byte at a time: the pending pieces stay a
    // short tower, and the event arrives intact.
    const p = sseClient.parser();
    const t0 = Date.now();
    p.feed("data: ");
    for (let i = 1; i <= 204800; i++) {
        p.feed(String.fromCharCode(97 + i % 26));
        if (i % 4096 === 0 && Date.now() - t0 > 3000)
            throw new Error("too slow: " + i + " bytes took over 3 s");
    }
    const evs = p.feed("\n\n");
    eq(evs.length, 1); eq(evs[0].data.length, 204800);
    eq(evs[0].data.slice(0, 3), "bcd");
    eq(evs[0].data[evs[0].data.length - 1], String.fromCharCode(97 + 204800 % 26));
});

test("sse_client_audit52.max_event_bytes", () => {
    // A line that never ends.
    let p = sseClient.parser({ maxEventBytes: 100 });
    p.feed("data: " + "x".repeat(80));
    eq(codeOf(() => p.feed("x".repeat(40))), "too_large");
    // Spent: a later feed throws again.
    eq(codeOf(() => p.feed("\n\n")), "too_large");
    // data lines with no blank line between them.
    p = sseClient.parser({ maxEventBytes: 100 });
    eq(codeOf(() => { for (let i = 0; i < 50; i++) p.feed("data: xyz\n"); }), "too_large");
    // The default is 1 MiB.
    p = sseClient.parser();
    eq(codeOf(() => p.feed("data: " + "x".repeat(1048576))), "too_large");
    // Within it nothing changes, and a dispatch starts the count again.
    p = sseClient.parser({ maxEventBytes: 100 });
    for (let i = 0; i < 5; i++) eq(p.feed("data: " + "y".repeat(90) + "\n\n").length, 1);
    // Bad options are refused.
    let threw = 0;
    try { sseClient.parser({ maxEventBytes: 0 }); } catch (e) { threw++; }
    try { sseClient.parser({ maxEventBytes: 1.5 }); } catch (e) { threw++; }
    eq(threw, 2);
});

test("sse_client_audit52.retry_persists", () => {
    const evs = run(["retry: 5000\n\ndata: a\n\nretry: 99999999999999999999\ndata: b\n\n"]);
    eq(evs.length, 2); eq(evs[0].retry, 5000); eq(evs[1].retry, 5000);
    eq(run(["data: a\n\n"])[0].retry, null);
});

test("sse_client_audit52.cr_at_chunk_end", () => {
    let p = sseClient.parser();
    eq(p.feed("data: a\r").length, 0);
    let evs = p.feed("\r");      // the blank line ends at once
    eq(evs.length, 1); eq(evs[0].data, "a");
    // An LF right after a chunk-ending CR is skipped, once.
    p = sseClient.parser();
    p.feed("data: b\r");
    evs = p.feed("\n\n");
    eq(evs.length, 1); eq(evs[0].data, "b");
    p = sseClient.parser();
    p.feed("data: c\r");
    eq(p.feed("\n").length, 0);
    eq(p.feed("\n").length, 1);
});

test("sse_client_audit52.empty_event_type", () => {
    const evs = run(["event:\ndata: x\n\n"]);
    eq(evs.length, 1); eq(evs[0].event, "message");
});

await atest("sse_client_audit52.events_too_large_closes", async () => {
    const res = fake(["data: " + "x".repeat(64)], null);
    const it = sseClient.events(res, { maxEventBytes: 16 });
    let code = null;
    try { await it.next(); } catch (e) { code = e.code; }
    eq(code, "too_large"); eq(res.closes, 1);
    eq((await it.next()).done, true);
});

await atest("sse_client_audit52.events_close_on_end", async () => {
    const res = fake(["data: 1\n\n"], null);
    const got = [];
    for await (const ev of sseClient.events(res)) got.push(ev.data);
    eq(got.length, 1); eq(res.closes, 1);
});

await atest("sse_client_audit52.events_close_on_error", async () => {
    const err = new Error("went quiet");
    err.code = "idle";
    const res = fake(["data: 1\n\n"], err);
    const it = sseClient.events(res);
    eq((await it.next()).value.data, "1");
    let code = null;
    try { await it.next(); } catch (e) { code = e.code; }
    eq(code, "idle"); eq(res.closes, 1);
    eq((await it.next()).done, true);
});

await atest("sse_client_audit52.concurrent_next", async () => {
    const res = fake(["data: 1\n\n", "data: 2\n\n"], null);
    const it = sseClient.events(res);
    const [a, b, c] = await Promise.all([it.next(), it.next(), it.next()]);
    eq(a.value.data, "1"); eq(b.value.data, "2"); eq(c.done, true);
    eq(res.closes, 1);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
