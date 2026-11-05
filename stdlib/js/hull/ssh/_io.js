// hull:ssh:_io - the SSH stack's byte stream (internal).
//
// The JS twin of what the Lua stack reaches as require('hull.ssh._stream'):
// the native module hull:ssh:_stream (src/hull/runtime/js/mod_ssh.c) moves
// bytes over a connection the capability (hl_cap_ssh_open, cap/ssh.c) has
// already checked against manifest.ssh. This wrapper gives it the duck-typed
// stream shape every hull:ssh:* layer takes (the same shape a test's fake
// stream and hull:ssh:ws_stream have):
//
//   await s.read(n)    -> [chunk] ("" at EOF) | [null, message, code]
//   await s.write(d)   -> [true] | [null, message, code]
//   s.close(); s.deadline(ms); s.wait(ms)
//
// The native calls refuse any caller that is not a stdlib module, and this
// module is underscore-named, so application code can import neither: an app
// asks for an SSH connection and never holds a socket.
//
// The native read / write / connect return their result at once when they
// can, and otherwise a Promise that settles (to nothing) once the stream has
// moved; the call is then made again. That is the Lua binding's park-and-
// retry (lua_yieldk + the same step), with the retry here: a read consumes
// nothing when it would wait, and a write is all-or-none, so neither can
// half-apply and then repeat.

// The native module is imported on first use, not at load: it exists only in a
// build with the outbound socket layer (HL_ENABLE_HTTP_CLIENT), and hull:ssh
// must still load without it - its codecs, trust stores and the openStream
// seam need no socket. Lua requires hull.ssh._stream lazily for the same
// reason. (The resolver refuses hull/ssh@1 to an app on such a build.)
let nativeConnect = null;
async function native() {
    if (!nativeConnect) {
        const m = await import("hull:ssh:_stream");
        nativeConnect = m.connect;
    }
    return nativeConnect;
}

// The native result, retried for as long as it reports "woken, try again".
// Each wait is bounded by the stream's own deadline / wait bound (an expiry
// comes back coded), so this loop always ends.
class Stream {
    constructor(h) {
        this.h = h;
    }

    async read(n) {
        for (;;) {
            const r = this.h.read(n);
            if (Array.isArray(r)) return r;
            await r;
        }
    }

    async write(data) {
        for (;;) {
            const r = this.h.write(data);
            if (Array.isArray(r)) return r;
            await r;
        }
    }

    close() { this.h.close(); }
    deadline(ms) { this.h.deadline(ms); }
    wait(ms) { this.h.wait(ms); }
}

// -> [stream] | [null, message, code]
//
// The native connect checks the grants and starts the dial, handing back the
// handle at once ([h], or [null, message, code] when refused); the handle's
// connectResult() then reports the dial: [true], [null, message, code], or a
// Promise to wait on before asking again.
async function connect(opts) {
    const r = (await native())(opts);
    if (r[0] === null) return r;
    const h = r[0];
    for (;;) {
        const s = h.connectResult();
        if (Array.isArray(s)) {
            if (s[0] === null) return s;
            return [new Stream(h)];
        }
        await s;
    }
}

export const _io = { connect };
export default _io;
