// hull:ssh:known_hosts - OpenSSH's known_hosts format, as text <-> entries.
//
// The JS twin of hull.ssh.known_hosts (stdlib/lua/hull/ssh/known_hosts.lua);
// see there for the format and what is deliberately not supported (patterns,
// @cert-authority). Pure: no files, no I/O. @revoked lines are honoured by
// revokedBlob. Byte strings throughout.

import { encoding } from "hull:encoding";
import { wire } from "hull:ssh:wire";

const base64 = encoding.base64;

const HASH_MAGIC = "|1|";

// Lua's %s: ASCII space, \t \n \v \f \r.
const isSpace = (c) => c === 32 || (c >= 9 && c <= 13);

// Linear trim of Lua %s whitespace (hull._text.trim).
function trim(s) {
    let i = 0, j = s.length;
    while (i < j && isSpace(s.charCodeAt(i))) i++;
    while (j > i && isSpace(s.charCodeAt(j - 1))) j--;
    return s.slice(i, j);
}

// Lua's string.lower: ASCII only.
function asciiLower(s) {
    return s.replace(/[A-Z]+/g, (m) => m.toLowerCase());
}

// The first `n` whitespace-separated fields at the start of `s` (which has no
// leading whitespace), or null when there are fewer.
function fields(s, n) {
    const out = [];
    let i = 0;
    while (out.length < n) {
        if (i >= s.length) return null;
        let j = i;
        while (j < s.length && !isSpace(s.charCodeAt(j))) j++;
        out.push(s.slice(i, j));
        while (j < s.length && isSpace(s.charCodeAt(j))) j++;
        if (out.length < n && j >= s.length) return null;
        i = j;
    }
    return out;
}

// The key type a wire-format key blob declares: its first string.
function blobType(blob) {
    try { return wire.reader(blob).string(); } catch (e) { return null; }
}

// One line, or null when it holds no usable entry. Never throws.
function parseLine(line) {
    const s = trim(line);
    if (s === "" || s[0] === "#" || s[0] === "@") return null;

    const f = fields(s, 3);
    if (!f) return null;
    const [hosts, keytype, b64] = f;
    const blob = base64.decode(b64);
    if (!blob || blob === "" || blobType(blob) !== keytype) return null;

    const e = { keytype, blob };
    if (hosts.slice(0, HASH_MAGIC.length) === HASH_MAGIC) {
        const m = /^\|1\|([^|]+)\|([^|]+)$/.exec(hosts);
        if (!m) return null;
        const salt = base64.decode(m[1]), hash = base64.decode(m[2]);
        if (!(salt && hash) || hash.length !== 20) return null;
        e.salt = salt;
        e.hash = hash;
    } else {
        e.names = [];
        for (const name of hosts.split(",")) {
            if (name === "") continue;
            // Patterns are skipped; plain names compare case-insensitively.
            if (!/[*?!]/.test(name)) e.names.push(asciiLower(name));
        }
        if (e.names.length === 0) return null;
    }
    return e;
}

// The key a "@revoked hosts keytype base64-key" line revokes, or null.
function revokedBlob(line) {
    const s = trim(line);
    if (s.slice(0, 8) !== "@revoked") return null;
    let i = 8;
    if (i >= s.length || !isSpace(s.charCodeAt(i))) return null;
    const f = fields(trim(s.slice(i)), 3);
    if (!f) return null;
    const keytype = f[1], b64 = f[2];
    const blob = base64.decode(b64);
    if (!blob || blob === "" || blobType(blob) !== keytype) return null;
    return blob;
}

// Whether entry `e` is for store name `name`. `hmacSha1(key, msg)` returns the
// raw 20-byte digest; it is only called for hashed entries.
function matches(e, name, hmacSha1) {
    if (e.names) {
        for (const n of e.names) if (n === name) return true;
        return false;
    }
    return hmacSha1(e.salt, name) === e.hash;
}

// The line recording `blob` for `name`: plain, or hashed with `salt`.
function render(name, blob, salt, hmacSha1) {
    const keytype = blobType(blob);
    if (!keytype) throw new TypeError("ssh.known_hosts: not a key blob");
    let hosts = name;
    if (!salt && (typeof name !== "string" || name === ""
                  || /[,#|*?!\t\n\v\f\r \x00-\x1f\x7f]/.test(name)
                  || (/[\[\]]/.test(name) && !/^\[[^\[\]]+\]:[0-9]+$/.test(name)))) {
        throw new TypeError("ssh.known_hosts: invalid host name");
    }
    if (salt) {
        hosts = HASH_MAGIC + base64.encode(salt) + "|" + base64.encode(hmacSha1(salt, name));
    }
    return hosts + " " + keytype + " " + base64.encode(blob);
}

// `text` without the entries for `name`.
function without(text, name, hmacSha1) {
    const out = [];
    const lines = (text + "\n").split("\n");
    lines.pop();     // the split after the added "\n"
    for (const line of lines) {
        const e = parseLine(line);
        if (e && matches(e, name, hmacSha1)) {
            if (e.names && e.names.length > 1) {
                const m = /^[\t\n\v\f\r ]*([^\t\n\v\f\r ]+)([^]*)$/.exec(line);
                const hosts = m[1], rest = m[2];
                const kept = [];
                for (const n of hosts.split(",")) {
                    if (n !== "" && asciiLower(n) !== name) kept.push(n);
                }
                if (kept.length > 0) out.push(kept.join(",") + rest);
            }
        } else {
            out.push(line);
        }
    }
    if (out.length > 0 && out[out.length - 1] === "") out.pop();
    return out.join("\n") + (out.length > 0 ? "\n" : "");
}

export const knownHosts = {
    HASH_MAGIC, blobType, parseLine, revokedBlob, matches, render, without,
};
export default knownHosts;
