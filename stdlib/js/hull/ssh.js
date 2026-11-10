// hull:ssh - the public SSH client (the JS twin of hull.ssh).
//
// The only module an application imports. Everything under hull:ssh:* is
// internal, and the byte stream itself is not reachable from application code
// at all (src/hull/runtime/js/mod_ssh.c): an app asks for an SSH connection
// and never holds a socket. The protocol is the line-for-line port of the Lua
// stack (stdlib/lua/hull/ssh/); the grants, the dial and the relay's TLS are
// decided by the same capability (hl_cap_ssh_open, cap/ssh.c).
//
//   import { ssh } from "hull:ssh";
//
//   const conn = await ssh.connect({
//       host: "spark-7468.example.com", user: "operator",
//       key: fs.read("id_ed25519"),          // or keys: [a, b], tried in order
//       trust: myStore,                      // get / put / forget, app-owned
//   });
//   const r = await conn.exec("uname -a");   // { status, signal, stdout, stderr }
//   await conn.exec("journalctl -fu app", { onStdout: (c) => log(c) });
//   conn.close();
//
// Reaching the host at all requires the manifest to say so:
//
//   ssh: { connect: { hosts: ["*.example.com"], ports: [22], users: ["operator"] } }
//
// and a host behind a WebSocket relay a second grant for the relay
// (ssh.tunnel). See stdlib/lua/hull/ssh.lua for the reasoning behind every
// rule here; this file keeps its structure.
//
// The JS shape (the one place it differs from Lua): every failure Lua returns
// as (nil, { code, detail, ... }) is THROWN here - an async method rejects -
// as an Error carrying `code`, `detail` and the same fields camelCased
// (fingerprint, storedFingerprint, keyBlob, methods, key, limit, status ...).
// An error raised by the caller's own callback, and a runtime refusal
// (`wait_refused` / `busy`, hull:_refusal), is rethrown unchanged. Argument
// misuse is a TypeError. Bytes are byte strings (code units 0..255, as
// hull:encoding); an input may also be an ArrayBuffer or typed array.

import { _refusal } from "hull:_refusal";
import { _test_hooks } from "hull:_test_hooks";
import { fs } from "hull:fs";
import { bytes } from "hull:ssh:bytes";
import { transport } from "hull:ssh:transport";
import { packet } from "hull:ssh:packet";
import { privatekey } from "hull:ssh:privatekey";
import { hostkey } from "hull:ssh:hostkey";
import { knownHosts } from "hull:ssh:known_hosts";
import { kexinit } from "hull:ssh:kexinit";
import { wsStream } from "hull:ssh:ws_stream";
import { _io } from "hull:ssh:_io";

const hasOwn = Object.prototype.hasOwnProperty;
const errText = (e) => (e && typeof e === "object" && typeof e.message === "string")
    ? e.message : String(e);

// The transport factory: transport.new, unless a C unit test swapped it in
// through hull:_test_hooks (the Lua suite patches the module's `new` field;
// the JS module object is frozen, and a seam on connect's options would hand
// app code the real stream).
let newTransport = transport.new;
_test_hooks.register("hull:ssh", {
    setTransportNew(fn) { newTransport = fn || transport.new; },
});

// Our coded errors: an Error with `code`, `detail` and the extra fields of a
// Lua reason table, marked so the guard can tell them from a runtime refusal.
function toError(r) {
    if (r instanceof Error) {
        if (!hasOwn.call(r, "ssh")) {
            Object.defineProperty(r, "ssh", { value: true, enumerable: false });
        }
        return r;
    }
    const detail = r.detail;
    const e = new Error(detail === undefined || detail === null
        ? String(r.code) : String(detail));
    for (const k of Object.keys(r)) e[k] = r[k];
    Object.defineProperty(e, "ssh", { value: true, enumerable: false });
    return e;
}

const isNil = (v) => v === undefined || v === null;

// A result as Lua's multiple returns, in the shape every hull:ssh:* layer
// hands back ([value] | [null, reason]); a lone value passes as it is.
function unwrap(r) {
    if (!Array.isArray(r)) return r;
    if (isNil(r[0]) && r.length > 1 && !isNil(r[1])) throw toError(r[1]);
    return isNil(r[0]) ? null : r[0];
}

// The one error shape (Lua's guard). The transport throws on I/O failure and
// malformed input and returns a reason for protocol refusals; the caller gets
// a coded Error for all of them. A coded failure keeps its code; the caller's
// own callback error ({ appError }) and a runtime refusal are rethrown
// unchanged; anything else is io_error.
function fault(e) {
    if (e !== null && typeof e === "object") {
        if (!(e instanceof Error) && hasOwn.call(e, "appError")) return e.appError;
        if (e instanceof Error && !hasOwn.call(e, "ssh") && _refusal.code(e) !== null) return e;
        if (e.code) return toError(e);
    }
    return toError({ code: "io_error", detail: errText(e) });
}

async function guard(fn) {
    let r;
    try {
        r = await fn();
    } catch (e) {
        throw fault(e);
    }
    return unwrap(r);
}

function guardSync(fn) {
    let r;
    try {
        r = fn();
    } catch (e) {
        throw fault(e);
    }
    return unwrap(r);
}

// A byte input: a byte string, or the bytes of an ArrayBuffer / typed array.
function bytesArg(v, what) {
    if (typeof v === "string") {
        if (!bytes.isBytes(v)) {
            throw new TypeError("ssh: " + what + " has a character above 0xFF; "
                + "pass text through encoding.utf8.encode");
        }
        return v;
    }
    if (v instanceof ArrayBuffer || ArrayBuffer.isView(v)) return bytes.fromBuf(v);
    throw new TypeError("ssh: " + what + " must be a byte string or a buffer");
}

const isBuf = (v) => v instanceof ArrayBuffer || ArrayBuffer.isView(v);

// A key file that could not be loaded, by what the caller would do about it.
function keyError(e) {
    if (e !== null && typeof e === "object" && e.code) {
        return { code: e.code, detail: e.detail };
    }
    return { code: "bad_key", detail: errText(e) };
}

/** Load a key from the contents of a key file (text or buffer).
 *  opts.passphraseEnv names an environment variable holding the passphrase
 *  (read and scrubbed in C - prefer it); opts.passphrase passes it directly. */
function loadKey(text, opts) {
    return privatekey.load(isBuf(text) ? bytes.fromBuf(text) : text, opts);
}

/** A single-use trust for ONE connection (platform gap 4): the first host key
 *  the server presents is the key that connection is held to (a rekey is
 *  pinned to it as on every connection), and nothing is stored anywhere. A
 *  second connect with the same object rejects `already_used`; acceptHost
 *  rejects it as a store (`not_a_store`). For enrollment: connect with it,
 *  authenticate, receive the node's proof, check it against conn.hostKey()
 *  and conn.sessionId() and the operator's out-of-band fingerprint, then
 *  conn.acceptHost(store) or conn.close(). See ssh.lua. */
function pendingTrust() {
    const p = { pending: true, used: false, key: null, name: null };
    p.get = (name) => (p.key !== null && p.name === name ? p.key : null);
    p.entries = () => (p.key !== null ? { [p.name]: p.key } : {});
    return p;
}

/** An in-memory trust store. */
function memoryStore(seed) {
    const t = Object.create(null);
    if (seed) for (const k of Object.keys(seed)) t[k] = seed[k];
    return {
        get: (host) => (hasOwn.call(t, host) ? t[host] : null),
        put: (host, blob) => { t[host] = blob; },
        forget: (host) => { delete t[host]; },
        entries: () => t,
    };
}

/** A trust store in an OpenSSH known_hosts file, through hull:fs. opts.hash
 *  writes new entries hashed. A missing file is an empty store; any other
 *  read failure throws `store_failed`. */
function fileStore(path, opts) {
    opts = opts || {};
    const fsm = opts.fs || fs;
    const crypto = opts.crypto || bytes.crypto;
    const kh = knownHosts;

    const hmac = (key, msg) => crypto.hmacSha1(msg, key);
    const load = () => {
        let ab;
        try {
            ab = fsm.read(path);
        } catch (e) {
            if (/not_found$/.test(errText(e))) return "";
            throw toError({ code: "store_failed", detail: errText(e) });
        }
        return bytes.fromBuf(ab);
    };
    const save = (text) => {
        try {
            fsm.write(path, bytes.toU8(text));
        } catch (e) {
            throw toError({ code: "store_failed", detail: errText(e) });
        }
    };
    const lines = (text) => text.split("\n").filter((l) => l !== "");
    const each = (fn) => {
        for (const line of lines(load())) {
            const e = kh.parseLine(line);
            if (e && fn(e)) return e;
        }
        return null;
    };

    const RANK = { "ssh-ed25519": 1, "ecdsa-sha2-nistp256": 2,
                   "ecdsa-sha2-nistp384": 3, "ssh-rsa": 4 };
    const rank = (t) => (hasOwn.call(RANK, t) ? RANK[t] : 0);

    return {
        get(name) {
            let best = null;
            each((x) => {
                const r = rank(x.keytype);
                if (r && (!best || r < rank(best.keytype)) && kh.matches(x, name, hmac)) {
                    best = x;
                }
                return false;
            });
            return best ? best.blob : null;
        },
        put(name, blob) {
            let text = load();
            if (text !== "" && text[text.length - 1] !== "\n") text += "\n";
            const salt = opts.hash ? crypto.random(16) : null;
            save(text + kh.render(name, blob, salt, hmac) + "\n");
        },
        forget(name) {
            save(kh.without(load(), name, hmac));
        },
        revoked(blob) {
            for (const line of lines(load())) {
                if (kh.revokedBlob(line) === blob) return true;
            }
            return false;
        },
        entries() {
            const out = Object.create(null);
            each((x) => {
                if (x.names && rank(x.keytype)) {
                    for (const n of x.names) {
                        const cur = hasOwn.call(out, n) ? hostkey.keyType(out[n]) : null;
                        if (!cur || rank(x.keytype) < rank(cur)) out[n] = x.blob;
                    }
                }
                return false;
            });
            return out;
        },
    };
}

/** A trust store in a hull:kv namespace; the backend must support
 *  compare-and-swap (and scan, to list entries). */
function kvStore(kv, opts) {
    opts = opts || {};
    if (kv === null || typeof kv !== "object" || typeof kv.cas !== "function"
        || !(kv.caps && kv.caps.compare_exchange)) {
        throw new TypeError("ssh.kvStore: needs a hull:kv handle whose backend "
            + "supports compare-and-swap");
    }
    const prefix = opts.prefix === undefined || opts.prefix === null ? "hostkey:" : opts.prefix;
    if (typeof prefix !== "string") {
        throw new TypeError("ssh.kvStore: prefix must be a string");
    }
    return {
        get: (name) => kv.get(prefix + name),
        put(name, blob) {
            if (!kv.cas(prefix + name, null, blob)) {
                throw toError({ code: "already_trusted",
                    detail: name + " already has a stored key; forgetHost it first" });
            }
        },
        forget: (name) => { kv.delete(prefix + name); },
        entries() {
            const out = Object.create(null);
            if (kv.caps.scan) {
                for (const key of kv.scan(prefix)) {
                    const blob = kv.get(key);
                    if (!isNil(blob)) out[key.slice(prefix.length)] = blob;
                }
            }
            return out;
        },
    };
}

// What an application holds is a HANDLE, never the machinery behind it: the
// transport (and an sftp session's) live in this map, keyed by the handle, and
// only the methods below can reach them.
const inner = new WeakMap();

function handle(proto, obj) {
    const h = Object.freeze(Object.create(proto));
    inner.set(h, obj);
    return h;
}

function self(h) {
    const o = inner.get(h);
    if (!o) throw new TypeError("ssh: not a handle");
    return o;
}

// An open file: read / write / seek / tell / stat / close.
const FileHandle = Object.freeze({
    read(n) { const o = self(this); return guard(() => o.read(n)); },
    write(data) {
        const o = self(this);
        return guard(() => o.write(bytesArg(data, "write data")));
    },
    seek(offset) { const o = self(this); return guardSync(() => o.seek(offset)); },
    tell() { const o = self(this); return guardSync(() => o.tell()); },
    stat() { const o = self(this); return guard(() => o.stat()); },
    close() { const o = self(this); return guard(() => o.close()); },
});

const pathArg = (p) => bytesArg(p, "path");

// An SFTP session (hull:ssh:sftp_client; docs/ssh.md for applications).
const SftpHandle = Object.freeze({
    realpath(p) { const o = self(this); return guard(() => o.realpath(pathArg(p))); },
    async list(p) {
        const o = self(this);
        const r = await guard(async () => {
            const res = await o.list(pathArg(p));
            // [safe, refused] (Lua's two returns) -> one object
            if (Array.isArray(res) && !isNil(res[0])) {
                return [{ entries: res[0], refused: res[1] || [] }];
            }
            return res;
        });
        return r;
    },
    read(p, max) { const o = self(this); return guard(() => o.read(pathArg(p), max)); },
    write(p, data) {
        const o = self(this);
        return guard(() => o.write(pathArg(p), bytesArg(data, "write data")));
    },
    stat(p) { const o = self(this); return guard(() => o.stat(pathArg(p))); },
    lstat(p) { const o = self(this); return guard(() => o.lstat(pathArg(p))); },
    mkdir(p, mode) { const o = self(this); return guard(() => o.mkdir(pathArg(p), mode)); },
    rmdir(p) { const o = self(this); return guard(() => o.rmdir(pathArg(p))); },
    remove(p) { const o = self(this); return guard(() => o.remove(pathArg(p))); },
    rename(from, to) {
        const o = self(this);
        return guard(() => o.rename(pathArg(from), pathArg(to)));
    },
    chmod(p, mode) { const o = self(this); return guard(() => o.chmod(pathArg(p), mode)); },
    setstat(p, attrs) { const o = self(this); return guard(() => o.setstat(pathArg(p), attrs)); },
    async open(p, mode, opts) {
        const o = self(this);
        const f = await guard(() => o.open(pathArg(p), mode, opts));
        return handle(FileHandle, f);
    },
    close() { return self(this).close(); },
});

// exec's options with byte inputs normalised: stdin a byte string (or a
// buffer), or a source function whose chunks may be buffers too.
function execOpts(opts) {
    if (isNil(opts)) return opts;
    if (typeof opts !== "object") return opts;
    const out = Object.assign({}, opts);
    const src = opts.stdin;
    if (typeof src === "string" || isBuf(src)) {
        out.stdin = bytesArg(src, "stdin");
    } else if (typeof src === "function") {
        out.stdin = async () => {
            const chunk = await src();
            if (typeof chunk === "string" || isBuf(chunk)) return bytesArg(chunk, "stdin chunk");
            return chunk;
        };
    }
    return out;
}

const ConnHandle = Object.freeze({
    /** Run a command -> { status, signal, stdout, stderr }. See ssh.lua. */
    exec(command, opts) {
        const o = self(this);
        return guard(() => o.exec(bytesArg(command, "command"), execOpts(opts)));
    },
    /** Open an SFTP session (opts.replyTimeoutMs, default 60000; 0 none). */
    async sftp(opts) {
        const o = self(this);
        const s = await guard(() => o.sftp(opts));
        return handle(SftpHandle, s);
    },
    close() { return self(this).close(); },
    fingerprint() { return self(this).hostFingerprint; },
    negotiated() { return self(this).negotiated; },
    /** Made with ssh.pendingTrust(): the host key is the one the server
     *  presented, not one a store vouched for. See ssh.lua. */
    trustPending() {
        const o = self(this).kexOpts;
        return !!(o && o.trust && typeof o.trust === "object" && o.trust.pending === true);
    },
    /** The host key this connection is bound to: { blob, type, algorithm,
     *  fingerprint } - blob the SSH wire encoding as a byte string (what
     *  acceptHost takes, what an enrollment proof binds), type the key type it
     *  declares, algorithm the signature algorithm negotiated for it. */
    hostKey() {
        const t = self(this);
        if (!t.hostKeyBlob) throw toError({ code: "not_handshaken" });
        return {
            blob: t.hostKeyBlob,
            type: hostkey.keyType(t.hostKeyBlob),
            algorithm: t.negotiated ? t.negotiated.hostKey : undefined,
            fingerprint: t.hostFingerprint,
        };
    },
    /** The RFC 4253 section 7.2 session identifier (the first exchange hash,
     *  fixed for the connection's life), as a byte string. */
    sessionId() {
        const t = self(this);
        if (!t.sessionId) throw toError({ code: "not_handshaken" });
        return t.sessionId;
    },
    /** Record this connection's host key in a real store, for the host and
     *  port it was dialled as (ssh.acceptHost with the connection's own key)
     *  -> true. Rejects like it, and rejects a pending trust (not_a_store). */
    acceptHost(store) {
        const t = self(this);
        if (!t.hostKeyBlob) return Promise.reject(toError({ code: "not_handshaken" }));
        const o = t.kexOpts || {};
        return acceptHost(store, o.host, t.hostKeyBlob, o.port);
    },
    /** Ask the server for new keys now -> true. Call BETWEEN operations. */
    rekey() { const o = self(this); return guard(() => o.rekey()); },
    /** { rekeys, authKey, bytesSent, bytesReceived, packetsSent,
     *    packetsReceived, rekeyDue } */
    stats() { return self(this).stats(); },
});

/** The budget for reaching an authenticated connection, unless
 *  opts.timeoutMs says otherwise. */
const CONNECT_TIMEOUT_MS = 30000;

const DEFAULT_DESTINATION_HEADER = "Cf-Access-Jump-Destination";

// Lua's %s set, trimmed linearly.
const isSpace = (c) => c === 32 || (c >= 9 && c <= 13);
function trim(s) {
    let i = 0, j = s.length;
    while (i < j && isSpace(s.charCodeAt(i))) i++;
    while (j > i && isSpace(s.charCodeAt(j - 1))) j--;
    return s.slice(i, j);
}

// host:port as a relay reads it; an IPv6 literal is bracketed. The port is
// the integer connect checked; the host one DNS name or IP literal.
function destinationValue(host, port) {
    if (!Number.isInteger(port)) return null;
    if (typeof host !== "string" || host === "" || host.length > 253
        || /[^A-Za-z0-9.\-_:]/.test(host)) {
        return null;
    }
    if (host.indexOf(":") >= 0) return "[" + host + "]:" + String(port);
    return host + ":" + String(port);
}

// The one sentence a person on a Mac needs when the dial itself says "no route
// to host" towards an address on their own network: macOS 15+ answers
// EHOSTUNREACH for a binary not yet allowed under Local Network privacy, and
// nothing in the error says so. Stdlib text, not C: the C layer reports the
// socket's errno and no more.
const MACOS_LOCAL_NETWORK_HINT =
    "on macOS 15+ a binary not yet allowed under Local Network privacy gets"
    + " this; allow it in System Settings > Privacy & Security > Local Network,"
    + " and again after every hull build";

// Whether `host` is an IP LITERAL in a private or link-local range (10/8,
// 172.16/12, 192.168/16, 169.254/16, fc00::/7, fe80::/10). Only a literal
// the app itself wrote is read: what a name resolved to never reaches this
// layer, and must not reach `detail`.
function isLocalIpLiteral(host) {
    if (typeof host !== "string") return false;
    let h = host.toLowerCase();
    const br = /^\[(.*)\]$/.exec(h);
    if (br) h = br[1];
    h = h.replace(/%.*$/, "");                     // an IPv6 zone index
    const v4 = /^(\d+)\.(\d+)\.(\d+)\.(\d+)$/.exec(h);
    if (v4) {
        const [a, b, c, d] = v4.slice(1).map(Number);
        if (a > 255 || b > 255 || c > 255 || d > 255) return false;
        return a === 10 || (a === 172 && b >= 16 && b <= 31)
            || (a === 192 && b === 168) || (a === 169 && b === 254);
    }
    if (h.indexOf(":") >= 0) {
        const first = /^([0-9a-f]{1,4}):/.exec(h);
        if (!first) return false;
        const v = parseInt(first[1], 16);
        return (v >= 0xfc00 && v <= 0xfdff) || (v >= 0xfe80 && v <= 0xfebf);
    }
    return false;
}

// The reason for a dial that failed: the binding's code and message, plus
// `errno` - the socket error's symbolic name ("EHOSTUNREACH", "ECONNREFUSED";
// absent for a DNS or TLS failure) - so an app branches on `e.errno`, never
// on `detail`. `dialled` is the host the app asked to have dialled: the relay
// with a tunnel, else the SSH host.
function dialFailure(code, detail, errnoName, dialled) {
    const e = { code: code || "connect_failed", detail };
    if (typeof errnoName === "string") {
        e.errno = errnoName;
        if (errnoName === "EHOSTUNREACH" && isLocalIpLiteral(dialled)) {
            e.detail = (isNil(detail) ? "" : String(detail) + "; ") + MACOS_LOCAL_NETWORK_HINT;
        }
    }
    return e;
}

// The headers to send: the caller's, with the destination header checked
// against the granted destination, or added -> [headers] | [null, reason].
// `tunnel` is tunnelOpener's snapshot: every header already a plain string.
function tunnelHeaders(tunnel, host, port) {
    let name = tunnel.destinationHeader;
    if (name === undefined || name === null) name = DEFAULT_DESTINATION_HEADER;
    const out = tunnel.headers.slice();

    const checked = new Set([DEFAULT_DESTINATION_HEADER.toLowerCase()]);
    if (name !== false) checked.add(name.toLowerCase());

    const want = destinationValue(host, port);
    if (!want) {
        return [null, { code: "denied", detail: "the destination must be a host "
            + "name or IP address and an integer port" }];
    }
    let seen = false;
    for (const line of out) {
        const s0 = String(line);
        const m = /^([A-Za-z0-9!#$%&'*+\-.^_`|~]+):/.exec(s0);
        if (!m) {
            return [null, { code: "denied", detail: "tunnel.headers entries are "
                + "'Name: value' lines; refused: " + s0.replace(/[\x00-\x1f\x7f]/g, "?") }];
        }
        const n0 = m[1].toLowerCase();
        if (n0 === "host" || n0 === "upgrade" || n0 === "connection"
            || n0.slice(0, 14) === "sec-websocket-") {
            return [null, { code: "denied", detail: "tunnel.headers may not set "
                + n0 + " (the relay is reached as the granted tunnel.host)" }];
        }
    }
    for (const line of out) {
        const s = String(line);
        const colon = s.indexOf(":");
        let n = null, v = null;
        if (colon >= 0) { n = trim(s.slice(0, colon)); v = trim(s.slice(colon + 1)); }
        if (n !== null && checked.has(n.toLowerCase())) {
            if (v !== want) {
                return [null, { code: "denied", detail: n + " names " + v
                    + ", but this connection is granted for " + want }];
            }
            if (name !== false && n.toLowerCase() === name.toLowerCase()) seen = true;
        }
    }
    if (name !== false && !seen) out.push(name + ": " + want);
    return [out];
}

// A stream opener's result as [stream, err, code]: the real one (and the
// relay dial) hands back an array, a test's opener may return the stream.
function streamResult(r) {
    return Array.isArray(r) ? r : [r];
}

// Reach the host through a WebSocket relay instead of dialling it directly.
// `dial` is how the RELAY is reached, defaulting to the capability-checked
// stream.
//
// Every option is read ONCE, here, into a plain snapshot that the rest uses:
// a getter read again could hand the grant check (the dial's via.host /
// tlsHostname) one name and the upgrade's Host header another, and a header
// that is not a primitive string got past the CR / LF checks and was turned
// into text, line breaks and all, only when the request was joined.
function tunnelOpener(tunnelIn, crypto, dial) {
    if (tunnelIn === null || typeof tunnelIn !== "object") {
        throw new TypeError("ssh.connect: tunnel must be an object");
    }
    const tunnel = {
        host: tunnelIn.host,
        tlsHostname: tunnelIn.tlsHostname,
        destinationHeader: tunnelIn.destinationHeader,
        path: tunnelIn.path,
        port: tunnelIn.port,
        tls: tunnelIn.tls,
        headers: tunnelIn.headers,
    };
    if (typeof tunnel.host !== "string" || tunnel.host === "") {
        throw new TypeError("ssh.connect: tunnel.host is required");
    }
    if (tunnel.tlsHostname === "") tunnel.tlsHostname = undefined;
    if (!isNil(tunnel.tlsHostname) && typeof tunnel.tlsHostname !== "string") {
        throw new TypeError("ssh.connect: tunnel.tlsHostname must be a host name");
    }
    if (!isNil(tunnel.port) && !Number.isInteger(tunnel.port)) {
        throw new TypeError("ssh.connect: tunnel.port must be an integer");
    }
    {
        const list = tunnel.headers;
        if (!isNil(list) && !Array.isArray(list)) {
            throw new TypeError("ssh.connect: tunnel.headers must be a list of 'Name: value' strings");
        }
        const n = isNil(list) ? 0 : list.length;
        const copy = [];
        for (let i = 0; i < n; i++) {
            const line = list[i];
            if (typeof line !== "string") {
                throw new TypeError("ssh.connect: tunnel.headers entries must be strings");
            }
            copy.push(line);
        }
        tunnel.headers = copy;
    }
    const dh = tunnel.destinationHeader;
    if (!isNil(dh) && dh !== false && (typeof dh !== "string" || dh === "")) {
        throw new TypeError("ssh.connect: tunnel.destinationHeader must be a header name or false");
    }
    const path = tunnel.path;
    if (!isNil(path) && (typeof path !== "string" || path[0] !== "/"
                         || path.slice(0, 2) === "//" || /[\s\x00-\x1f\x7f]/.test(path))) {
        throw new TypeError("ssh.connect: tunnel.path must be a path starting with one '/'"
            + " and holding no spaces or control characters");
    }
    const port = isNil(tunnel.port) ? 443 : tunnel.port;
    dial = dial || ((o) => _io.connect(o));
    // TLS unless the caller explicitly says otherwise.
    const tls = tunnel.tls !== false;

    return async (o) => {
        const [headers, herr] = tunnelHeaders(tunnel, o.host, o.port);
        if (!headers) return [null, herr];

        const [raw, err, code, errnoName] = streamResult(await dial({
            host: o.host,
            port: o.port,
            user: o.user,
            timeoutMs: o.timeoutMs,
            via: {
                host: tunnel.host,
                port,
                tls,
                tlsHostname: tunnel.tlsHostname,
            },
        }));
        // The errno is the RELAY's: that is the machine the socket reached for.
        if (!raw) return [null, dialFailure(code, err, errnoName, tunnel.host)];

        // The dial's bound ends at TCP + TLS; the upgrade's reply would wait
        // with none (a relay that never answers parked the caller for good).
        // The transport sets its own deadline once the stream is handed over.
        // Every way out that is not a stream drops the socket, a throw (a
        // runtime refusal of the upgrade's write or read) included: it was
        // left open until collected (audit 44).
        let s, werr;
        try {
            if (Number.isInteger(o.timeoutMs) && o.timeoutMs > 0 && raw.deadline) {
                raw.deadline(o.timeoutMs);
            }
            [s, werr] = await wsStream.connect(raw, {
                host: tunnel.tlsHostname || tunnel.host,
                path: tunnel.path,
                headers,
                random: crypto.random,
                sha1: crypto.sha1,
            });
        } catch (e) {
            try { raw.close(); } catch (e2) { /* the original error goes on */ }
            throw e;
        }
        if (!s) {
            raw.close();
            return [null, werr];
        }
        return [s];
    };
}

/** Open a connection. Resolves to a connection handle, or rejects with a
 *  coded Error: host_unknown (fingerprint, keyBlob), host_changed
 *  (fingerprint, storedFingerprint, keyBlob), host_revoked, already_used (a
 *  pendingTrust used before), auth_failed
 *  (methods), partial_success, denied, connect_failed (errno: the socket
 *  error's name, "EHOSTUNREACH", absent for a DNS or relay-TLS failure),
 *  timeout, too_many_streams, bad_key (key: 0-based index into keys), ... */
async function connect(opts) {
    if (opts === null || typeof opts !== "object") {
        throw new TypeError("ssh.connect: expected an options object");
    }
    for (const req of ["host", "user"]) {
        if (isNil(opts[req])) throw new TypeError("ssh.connect: " + req + " is required");
    }
    if (isNil(opts.key) === isNil(opts.keys)) {
        throw new TypeError("ssh.connect: exactly one of key and keys is required");
    }
    if (!isNil(opts.keys) && (!Array.isArray(opts.keys) || opts.keys.length === 0)) {
        throw new TypeError("ssh.connect: keys must be a non-empty list");
    }
    const [offerOk, offerWhy] = kexinit.validateOffer(opts.offer);
    if (!offerOk) throw new TypeError("ssh.connect: " + offerWhy);

    const crypto = opts.crypto || bytes.crypto;
    // Every key is loaded before anything is dialled. Each entry is key-file
    // text (a string or buffer) or a key already loaded with ssh.loadKey.
    const keys = [];
    const list = isNil(opts.keys) ? [opts.key] : opts.keys;
    for (let i = 0; i < list.length; i++) {
        let k = list[i];
        if (k === null || typeof k !== "object" || isBuf(k)) {
            try {
                k = privatekey.load(isBuf(k) ? bytes.fromBuf(k) : k, {
                    passphrase: opts.passphrase,
                    passphraseEnv: opts.passphraseEnv,
                    crypto,
                });
            } catch (e) {
                const r = keyError(e);
                if (!isNil(opts.keys)) r.key = i;   // 0-based (Lua: 1-based)
                throw toError(r);
            }
        }
        keys.push(k);
    }
    const trust = opts.trust || memoryStore();

    const timeout = isNil(opts.timeoutMs) ? CONNECT_TIMEOUT_MS : opts.timeoutMs;
    if (!isNil(opts.port) && !Number.isInteger(opts.port)) {
        throw new TypeError("ssh.connect: port must be an integer");
    }
    if (!Number.isInteger(timeout)) {
        throw new TypeError("ssh.connect: timeoutMs must be an integer");
    }
    if (opts.tunnel !== null && typeof opts.tunnel === "object"
        && !isNil(opts.tunnel.port) && !Number.isInteger(opts.tunnel.port)) {
        throw new TypeError("ssh.connect: tunnel.port must be an integer");
    }
    if (!isNil(opts.software)) {
        try {
            packet.buildIdent(opts.software);
        } catch (e) {
            throw toError({ code: "bad_software", detail: errText(e) });
        }
    }

    let open;
    if (!isNil(opts.tunnel)) {
        open = tunnelOpener(opts.tunnel, crypto, opts.openStream);
    } else if (opts.openStream) {
        open = opts.openStream;
    } else {
        open = (o) => _io.connect(o);
    }

    // A pending trust is for ONE connection (hull:ssh:hostkey check); taken
    // at the connect, success or not, before anything is dialled - and only
    // once every argument has been checked, so a connect refused for its own
    // arguments does not spend it (audit 44).
    if (trust !== null && typeof trust === "object" && trust.pending === true) {
        if (trust.used) {
            throw toError({ code: "already_used", detail: "a pending trust is for one "
                + "connection; make another with ssh.pendingTrust()" });
        }
        trust.used = true;
    }

    const port = isNil(opts.port) ? 22 : opts.port;
    const [stream, serr, scode, serrno] = streamResult(await open({
        host: opts.host, port, user: opts.user, timeoutMs: timeout,
    }));
    if (!stream) {
        // A reason that already carries a code keeps it (the tunnel path's
        // `upgrade_refused`); otherwise the binding's code says which
        // happened, and its fourth value - the socket errno's name, when the
        // dial itself failed - becomes `e.errno`.
        if (serr !== null && typeof serr === "object" && serr.code) throw toError(serr);
        throw toError(dialFailure(scode, serr, serrno, opts.host));
    }

    const t = newTransport(stream, crypto, {
        software: opts.software,
        keepaliveMs: opts.keepaliveMs,
        keepaliveMax: opts.keepaliveMax,
        idleMs: opts.idleMs,
    });
    t.setDeadline(timeout);

    const step = async (fn) => {
        try {
            return await guard(fn);
        } catch (e) {
            if (e && e.code === "deadline" && hasOwn.call(e, "ssh")) {
                throw toError({ code: "timeout", detail: "not connected and "
                    + "authenticated within " + String(timeout) + " ms" });
            }
            throw e;
        }
    };

    try {
        await step(() => t.handshake({
            host: opts.host, port, trust, offer: opts.offer, onBanner: opts.onBanner,
        }));
        await step(() => t.authenticate(opts.user, keys, opts.onBanner));
    } catch (e) {
        t.close();
        throw e;
    }

    t.setDeadline(0);   // connected: from here on, liveness is keepalives
    return handle(ConnHandle, t);
}

/** Accept a host key the caller has decided to trust (the key is trusted for
 *  the host AND port, default 22) -> true. Rejects with host_key_invalid,
 *  host_revoked, already_trusted, or the store's own failure. */
async function acceptHost(trust, host, keyBlob, port) {
    const name = hostkey.storeName(host, port);
    return guard(async () => {
        let blob;
        try { blob = bytesArg(keyBlob, "keyBlob"); } catch (_) { blob = null; }
        let parsed = false;
        if (blob !== null) {
            try { hostkey.parseKey(blob); parsed = true; } catch (_) { parsed = false; }
        }
        if (!parsed) {
            throw toError({ code: "host_key_invalid",
                detail: "not a host key blob Hull verifies (ssh-ed25519, "
                    + "ecdsa-sha2-nistp256/384 or ssh-rsa)" });
        }
        if (trust !== null && typeof trust === "object" && trust.pending === true) {
            throw toError({ code: "not_a_store", detail: "a pending trust holds one "
                + "connection's key and persists nothing; pass the real store" });
        }
        if (trust !== null && typeof trust === "object" && typeof trust.revoked === "function"
            && await trust.revoked(blob)) {
            throw toError({ code: "host_revoked", detail: "the trust store revokes this key" });
        }
        if (trust !== null && typeof trust === "object" && typeof trust.get === "function"
            && !isNil(await trust.get(name))) {
            throw toError({ code: "already_trusted", detail: name
                + " already has a stored key; forgetHost it first" });
        }
        return hostkey.acceptNew(trust, name, blob);
    });
}

/** Forget the key stored for host (and port, default 22) -> true. */
async function forgetHost(trust, host, port) {
    const name = hostkey.storeName(host, port);
    return guard(() => hostkey.forget(trust, name));
}

/** Fingerprint a key blob: SHA256:<base64>, what `ssh-keygen -l` prints. */
function fingerprint(keyBlob, opts) {
    const crypto = (opts && opts.crypto) || bytes.crypto;
    return hostkey.fingerprint(crypto.sha256, bytesArg(keyBlob, "keyBlob"));
}

export const ssh = {
    loadKey, pendingTrust, memoryStore, fileStore, kvStore, connect, acceptHost,
    forgetHost, fingerprint, CONNECT_TIMEOUT_MS,
};
export default ssh;
