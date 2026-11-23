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
 *       // ev.id (the last event id seen), ev.retry (the reconnection time
 *       // in ms the server last set, or null)
 *   }
 *
 * events(res, opts) is an async iterator of events, done once the stream
 * ended. The stream is closed when the iterator reaches the end or an error,
 * and when the loop is left early (break, throw). Calls to next() made
 * before the previous one settled wait their turn. A stream that ends with
 * an error throws the read's own Error (its `code`: "idle", "overflow",
 * "too_large", "closed", "io_error", ...). An event the stream ended in the
 * middle of is dropped, as the standard says.
 *
 * One event is capped: its pending line plus the data it gathered so far may
 * not pass opts.maxEventBytes (default 1 MiB, counted in UTF-16 code units);
 * past it the parser throws an Error with code "too_large" (events() closes
 * the stream first). A parser that threw it is spent: every later feed
 * throws again.
 *
 * sseClient.parser(opts) is the parser alone: feed(text) returns the events
 * the text completes, finish() ends the input. Lines end at CR LF, LF or CR
 * (a CR ends its line at once; an LF right after it, in the next chunk too,
 * is skipped); a leading byte-order mark is skipped; comment lines (":") are
 * ignored; `data` lines join with LF; an `id` holding a NUL is ignored;
 * `retry` takes digits only (a value past 2^53 - 1 is ignored) and stays the
 * reconnection time until another one arrives. A line split over many
 * chunks is scanned once, and its pieces are kept as a tower of O(log n)
 * strings (O(n log n) copying).
 *
 * @license AGPL-3.0-or-later
 */

const DEFAULT_MAX_EVENT_BYTES = 1048576;

function coded(code, message) {
    const e = new Error("sse-client: " + message);
    e.code = code;
    return e;
}

function maxEventBytes(opts) {
    if (opts === undefined || opts === null) return DEFAULT_MAX_EVENT_BYTES;
    if (typeof opts !== "object")
        throw new TypeError("sse-client: opts must be an object");
    const n = opts.maxEventBytes;
    if (n === undefined) return DEFAULT_MAX_EVENT_BYTES;
    if (!Number.isSafeInteger(n) || n < 1)
        throw new TypeError("sse-client: maxEventBytes must be a positive integer");
    return n;
}

function newParser(limit) {
    let started = false;
    let pending = [];       // pieces of the line not yet ended (a tower)
    let pendingLen = 0;
    let skipLf = false;     // the last chunk ended a line at a CR
    let data = null;        // array of data lines, null = no data field yet
    let dataLen = 0;
    let event = null;
    let lastId = null;
    let retry = null;       // the reconnection time, kept across events
    let out = null;
    let failed = null;

    function tooLarge() {
        failed = coded("too_large", "an event passed maxEventBytes (" + limit + ")");
        throw failed;
    }

    function dispatch() {
        if (data !== null) {
            out.push({ event: event === null || event === "" ? "message" : event,
                       data: data.join("\n"), id: lastId, retry });
        }
        data = null;
        dataLen = 0;
        event = null;
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
            if (data !== null) {
                dataLen += 1 + value.length;
            } else {
                data = [];
                dataLen = value.length;
            }
            if (dataLen > limit) tooLarge();
            data.push(value);
        } else if (name === "event") {
            event = value;
        } else if (name === "id") {
            if (value.indexOf("\0") < 0) lastId = value;
        } else if (name === "retry") {
            if (/^[0-9]+$/.test(value)) {
                const n = Number(value);
                if (Number.isSafeInteger(n)) retry = n;
            }
        }
    }

    // Scan one chunk once: every line it ends is the pending pieces joined
    // with the chunk's part, and the rest of the chunk becomes pending.
    function scan(text) {
        let pos = 0;
        const n = text.length;
        if (skipLf && n > 0) {
            if (text.charCodeAt(0) === 10) pos = 1;
            skipLf = false;
        }
        while (pos < n) {
            let s = -1;
            for (let i = pos; i < n; i++) {
                const c = text.charCodeAt(i);
                if (c === 13 || c === 10) { s = i; break; }
            }
            if (s < 0) break;
            if (pendingLen + (s - pos) + dataLen > limit) tooLarge();
            let line = text.slice(pos, s);
            if (pendingLen > 0) {
                pending.push(line);
                line = pending.join("");
                pending = [];
                pendingLen = 0;
            }
            field(line);
            if (text.charCodeAt(s) === 13) {
                if (s === n - 1) {
                    skipLf = true;
                } else if (text.charCodeAt(s + 1) === 10) {
                    s++;
                }
            }
            pos = s + 1;
        }
        if (pos < n) {
            pendingLen += n - pos;
            if (pendingLen + dataLen > limit) tooLarge();
            // A tower of pieces (PIL 11.6): a piece not shorter than the one
            // below merges into it, so the list holds O(log n) strings even
            // when the line arrives a byte at a time, and the total copying
            // stays O(n log n).
            pending.push(text.slice(pos));
            while (pending.length >= 2 &&
                   pending[pending.length - 1].length >= pending[pending.length - 2].length) {
                const top = pending.pop();
                pending[pending.length - 1] = pending[pending.length - 1] + top;
            }
        }
    }

    return {
        feed(text) {
            if (typeof text !== "string")
                throw new TypeError("sse-client: feed takes a string");
            if (failed !== null) throw failed;
            if (!started) {
                // Text arrives decoded, so a BOM is one whole code unit.
                if (text === "") return [];
                started = true;
                if (text.charCodeAt(0) === 0xFEFF) text = text.slice(1);
            }
            out = [];
            scan(text);
            const r = out;
            out = null;
            return r;
        },
        finish() {
            if (failed !== null) throw failed;
            // Whatever is pending is an event the input ended inside: dropped.
            // (A line ends at its CR at once, so nothing is left to complete.)
            pending = [];
            pendingLen = 0;
            skipLf = false;
            data = null;
            dataLen = 0;
            event = null;
            return [];
        },
    };
}

function parser(opts) {
    return newParser(maxEventBytes(opts));
}

// An async iterator object rather than an async generator: the project
// analyzer (stdlib/cli/js/hull/source) declines generators.
function events(res, opts) {
    if (res === null || typeof res !== "object" || typeof res.readText !== "function")
        throw new TypeError("sse-client: events takes a response from httpClient.async.stream");
    const p = newParser(maxEventBytes(opts));
    let queue = [];
    let qi = 0;
    let done = false;
    let closed = false;
    let chain = Promise.resolve();
    function close() {
        if (closed) return;
        closed = true;
        if (typeof res.close === "function") res.close();
    }
    // Stop on an error: close the stream without letting a failing close
    // replace the error being thrown.
    function fail(e) {
        done = true;
        queue = [];
        qi = 0;
        if (!closed) {
            closed = true;
            try { if (typeof res.close === "function") res.close(); } catch (_) { /* keep e */ }
        }
        throw e;
    }
    async function step() {
        for (;;) {
            if (qi < queue.length) return { value: queue[qi++], done: false };
            if (done) return { value: undefined, done: true };
            let text;
            try {
                text = await res.readText();
            } catch (e) {
                fail(e);
            }
            if (text === null) {
                queue = p.finish();
                qi = 0;
                done = true;
                close();
            } else {
                let evs;
                try { evs = p.feed(text); } catch (e) { fail(e); }
                queue = evs;
                qi = 0;
            }
        }
    }
    // Calls made before the previous one settled queue behind it: one read
    // at a time per stream.
    function serial(fn) {
        const r = chain.then(fn);
        chain = r.then(() => undefined, () => undefined);
        return r;
    }
    const it = {
        next() { return serial(step); },
        // Not queued: closing at once also ends a read a next() waits on.
        async return() {
            done = true;
            queue = [];
            qi = 0;
            close();
            return { value: undefined, done: true };
        },
        [Symbol.asyncIterator]() { return it; },
    };
    return it;
}

export const sseClient = { events, parser };
export default sseClient;
