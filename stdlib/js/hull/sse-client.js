/**
 * @file hull:sse-client
 * @module hull:sse-client
 * @description Read a Server-Sent Events stream.
 *
 * Mirror of stdlib/lua/hull/sse-client.lua: the event-stream parser of the
 * HTML standard ("9.2 Server-sent events"), over a response from
 * httpClient.async.stream:
 *
 *   import { httpClient } from "hull:http-client";
 *   import { sseClient } from "hull:sse-client";
 *   const res = await httpClient.async.stream("GET", "https://api.example.com/feed",
 *       { headers: { accept: "text/event-stream" } });
 *   for await (const ev of sseClient.events(res)) {
 *       // ev.event ("message" unless the server named one), ev.data,
 *       // ev.id (the last event id seen), ev.retry (ms, when sent)
 *   }
 *
 * events(res) is an async iterator of events, done once the stream ended. A
 * stream that ends with an error throws the read's own Error (its `code`:
 * "idle", "overflow", "too_large", "closed", "io_error", ...). An event the
 * stream ended in the middle of is dropped, as the standard says. Leaving the
 * loop early (break, throw) closes the stream.
 *
 * sseClient.parser() is the parser alone: feed(text) returns the events the
 * text completes, finish() ends the input and returns the events a trailing
 * CR completed. Lines end at CR LF, LF or CR (a CR at the end of one chunk
 * waits for the next); a leading byte-order mark is skipped; comment lines
 * (":") are ignored; `data` lines join with LF; an `id` holding a NUL is
 * ignored; `retry` takes digits only.
 *
 * @license AGPL-3.0-or-later
 */

function newParser() {
    let buf = "";
    let started = false;
    let data = null;        // array of data lines, null = no data field yet
    let event = null;
    let lastId = null;
    let retry = null;
    let out = null;

    function dispatch() {
        if (data !== null) {
            out.push({ event: event === null ? "message" : event,
                       data: data.join("\n"), id: lastId, retry });
        }
        data = null;
        event = null;
        retry = null;
    }

    function field(line) {
        if (line === "") { dispatch(); return; }
        if (line[0] === ":") return;
        let name, value;
        const colon = line.indexOf(":");
        if (colon >= 0) {
            name = line.slice(0, colon);
            value = line.slice(colon + 1);
            if (value[0] === " ") value = value.slice(1);
        } else {
            name = line;
            value = "";
        }
        if (name === "data") {
            if (data === null) data = [];
            data.push(value);
        } else if (name === "event") {
            event = value;
        } else if (name === "id") {
            if (value.indexOf("\0") < 0) lastId = value;
        } else if (name === "retry") {
            if (/^[0-9]+$/.test(value)) retry = Number(value);
        }
    }

    function drain(final) {
        let pos = 0;
        const n = buf.length;
        while (pos < n) {
            let s = -1;
            for (let i = pos; i < n; i++) {
                const c = buf.charCodeAt(i);
                if (c === 13 || c === 10) { s = i; break; }
            }
            if (s < 0) break;
            const cr = buf.charCodeAt(s) === 13;
            if (cr && s === n - 1 && !final) break;
            field(buf.slice(pos, s));
            if (cr && buf.charCodeAt(s + 1) === 10) s++;
            pos = s + 1;
        }
        buf = buf.slice(pos);
    }

    return {
        feed(text) {
            if (typeof text !== "string")
                throw new TypeError("sse-client: feed takes a string");
            if (!started) {
                if (text === "") return [];
                started = true;
                if (text.charCodeAt(0) === 0xFEFF) text = text.slice(1);
            }
            out = [];
            buf += text;
            drain(false);
            const r = out;
            out = null;
            return r;
        },
        finish() {
            out = [];
            drain(true);            // a trailing CR ends its line now
            const r = out;
            // Whatever is left is an event the input ended inside: dropped.
            out = null;
            buf = "";
            data = null;
            event = null;
            retry = null;
            return r;
        },
    };
}

function parser() {
    return newParser();
}

// An async iterator object rather than an async generator: the project
// analyzer (stdlib/cli/js/hull/source) declines generators.
function events(res) {
    if (res === null || typeof res !== "object" || typeof res.readText !== "function")
        throw new TypeError("sse-client: events takes a response from httpClient.async.stream");
    const p = newParser();
    let queue = [];
    let qi = 0;
    let done = false;
    const it = {
        async next() {
            for (;;) {
                if (qi < queue.length) return { value: queue[qi++], done: false };
                if (done) return { value: undefined, done: true };
                let text;
                try {
                    text = await res.readText();
                } catch (e) {
                    done = true;     // the stream ended with this error
                    queue = [];
                    throw e;
                }
                queue = text === null ? p.finish() : p.feed(text);
                qi = 0;
                if (text === null) done = true;
            }
        },
        async return() {
            if (!done) { done = true; res.close(); }
            queue = [];
            return { value: undefined, done: true };
        },
        [Symbol.asyncIterator]() { return it; },
    };
    return it;
}

export const sseClient = { events, parser };
export default sseClient;
