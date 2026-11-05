// hull:ssh:wire - the SSH binary data types (RFC 4251 section 5).
//
// The JS twin of hull.ssh.wire (stdlib/lua/hull/ssh/wire.lua); see there for
// the reasoning behind each rule. Every byte an SSH peer sends arrives
// through this module, most of it before authentication has happened.
//
// Readers THROW on malformed input rather than returning null: a protocol
// parser threads dozens of reads together, and the caller catches once at the
// packet boundary.
//
// All values are byte strings (one code unit per byte, hull:ssh:bytes);
// nothing here interprets text. Positions are 0-based.
//
// Deviation from Lua, by representation: a uint64 is a JS number, so values
// above 2^53 - 1 (Number.MAX_SAFE_INTEGER) are refused where Lua refuses
// above 2^63 - 1. No SSH or SFTP length, offset or size this client handles
// comes near either bound.

const MAX_U64 = Number.MAX_SAFE_INTEGER;

function u32be(v) {
    return String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
}

function isInt(v) {
    return typeof v === "number" && Number.isInteger(v);
}

// Writer -------------------------------------------------------------

// A new packet builder. Parts accumulate and are joined once, so building a
// packet is linear rather than quadratic in its length.
class Writer {
    constructor() {
        this.parts = [];
    }

    raw(s) {
        this.parts.push(s);
        return this;
    }

    byte(v) {
        if (!isInt(v) || v < 0 || v > 255) {
            throw new RangeError("ssh.wire: byte out of range: " + String(v));
        }
        return this.raw(String.fromCharCode(v));
    }

    boolean(v) {
        // RFC 4251: any non-zero value reads as true, but a sender MUST emit 1.
        return this.raw(v ? "\x01" : "\x00");
    }

    uint32(v) {
        if (!isInt(v) || v < 0 || v > 0xFFFFFFFF) {
            throw new RangeError("ssh.wire: uint32 out of range: " + String(v));
        }
        return this.raw(u32be(v));
    }

    uint64(v) {
        if (!isInt(v) || v < 0 || v > MAX_U64) {
            throw new RangeError("ssh.wire: uint64 out of range: " + String(v));
        }
        const hi = Math.floor(v / 0x100000000);
        const lo = v - hi * 0x100000000;
        return this.raw(u32be(hi) + u32be(lo));
    }

    // A length-prefixed binary blob. Not text: no encoding is applied.
    string(s) {
        if (typeof s !== "string") {
            throw new TypeError("ssh.wire: string expected, got " + typeof s);
        }
        return this.raw(u32be(s.length)).raw(s);
    }

    // A multiple-precision integer, given as a big-endian unsigned magnitude.
    // Only non-negative values are supported.
    mpint(mag) {
        if (typeof mag !== "string") {
            throw new TypeError("ssh.wire: mpint expects a magnitude string");
        }
        // RFC 4251: unnecessary leading zero bytes MUST NOT be included.
        let i = 0;
        while (i < mag.length && mag.charCodeAt(i) === 0) i++;
        let trimmed = mag.slice(i);
        // ...and zero is the empty string, not a zero byte.
        if (trimmed.length === 0) return this.raw(u32be(0));
        // A leading byte of 0x80 or more would read as negative.
        if (trimmed.charCodeAt(0) >= 0x80) trimmed = "\x00" + trimmed;
        return this.raw(u32be(trimmed.length)).raw(trimmed);
    }

    // A comma-separated algorithm list, length-prefixed as one string.
    namelist(names) {
        if (!Array.isArray(names)) {
            throw new TypeError("ssh.wire: namelist expects an array");
        }
        for (const n of names) {
            if (typeof n !== "string" || n === "") {
                throw new TypeError("ssh.wire: namelist entries must be non-empty strings");
            }
            if (n.indexOf(",") >= 0) {
                // A comma inside a name splits into two names on the wire.
                throw new TypeError("ssh.wire: namelist entry contains a comma: " + n);
            }
        }
        return this.string(names.join(","));
    }

    build() {
        return this.parts.join("");
    }

    // Bytes written so far, without building.
    len() {
        let t = 0;
        for (const p of this.parts) t += p.length;
        return t;
    }
}

function writer() {
    return new Writer();
}

// Reader -------------------------------------------------------------

// A cursor over buf. pos is 0-based and points at the next unread byte.
class Reader {
    constructor(buf) {
        this.buf = buf;
        this.pos = 0;
    }

    remaining() {
        return this.buf.length - this.pos;
    }

    // Every read funnels through here, so the bounds check exists exactly once.
    need(n) {
        if (n < 0 || this.remaining() < n) {
            throw new Error("ssh.wire: truncated: need " + String(n)
                + " byte(s), have " + String(this.remaining()));
        }
    }

    raw(n) {
        this.need(n);
        const s = this.buf.slice(this.pos, this.pos + n);
        this.pos += n;
        return s;
    }

    byte() {
        this.need(1);
        const v = this.buf.charCodeAt(this.pos);
        this.pos += 1;
        return v;
    }

    boolean() {
        // RFC 4251: zero is false and EVERY non-zero value is true.
        return this.byte() !== 0;
    }

    uint32() {
        this.need(4);
        const b = this.buf, p = this.pos;
        const v = ((b.charCodeAt(p) << 24) >>> 0) + (b.charCodeAt(p + 1) << 16)
            + (b.charCodeAt(p + 2) << 8) + b.charCodeAt(p + 3);
        this.pos += 4;
        return v;
    }

    uint64() {
        this.need(8);
        const hi = this.uint32();
        const lo = this.uint32();
        const v = hi * 0x100000000 + lo;
        if (v > MAX_U64) {
            // A value a JS number cannot hold exactly. Refuse rather than
            // hand back a rounded length or offset.
            throw new Error("ssh.wire: uint64 exceeds the representable range");
        }
        return v;
    }

    string() {
        const n = this.uint32();
        // Bounds are checked BEFORE anything of size n is touched.
        this.need(n);
        return this.raw(n);
    }

    // The big-endian unsigned magnitude with leading zeros stripped.
    mpint() {
        const n = this.uint32();
        this.need(n);
        const s = this.raw(n);
        if (s.length === 0) return "";
        if (s.charCodeAt(0) >= 0x80) {
            throw new Error("ssh.wire: negative mpint is not supported");
        }
        let i = 0;
        while (i < s.length - 1 && s.charCodeAt(i) === 0) i++;
        return s.slice(i);
    }

    namelist() {
        const s = this.string();
        const out = [];
        if (s === "") return out;      // an empty list is legal
        let i = 0;
        for (;;) {
            const j = s.indexOf(",", i);
            const name = j >= 0 ? s.slice(i, j) : s.slice(i);
            if (name === "") throw new Error("ssh.wire: empty name in namelist");
            // RFC 4251 section 6: names are printable US-ASCII.
            if (/[^\x20-\x7e]/.test(name)) {
                throw new Error("ssh.wire: namelist entry is not printable US-ASCII");
            }
            out.push(name);
            if (j < 0) break;
            i = j + 1;
        }
        return out;
    }
}

function reader(buf) {
    if (typeof buf !== "string") {
        throw new TypeError("ssh.wire: reader expects a string");
    }
    return new Reader(buf);
}

// Peer text headed for a terminal -------------------------------------

// Code points a terminal acts on rather than shows, beyond C0 and DEL.
function isInvisibleControl(cp) {
    return (cp >= 0x80 && cp <= 0x9F)
        || (cp >= 0x202A && cp <= 0x202E)
        || (cp >= 0x2066 && cp <= 0x2069)
        || cp === 0x200E || cp === 0x200F;
}

// The code point of a strict UTF-8 sequence of `len` bytes at `i` (overlongs,
// surrogates and values past U+10FFFF rejected, as Lua's utf8.codepoint), or
// -1 when it is not one.
function decodeAt(s, i, len) {
    const c = s.charCodeAt(i);
    if (len === 1) return c;
    let cp = c & (len === 2 ? 0x1F : len === 3 ? 0x0F : 0x07);
    for (let k = 1; k < len; k++) {
        const t = s.charCodeAt(i + k);
        if (t < 0x80 || t > 0xBF) return -1;
        cp = (cp << 6) | (t & 0x3F);
    }
    const min = len === 2 ? 0x80 : len === 3 ? 0x800 : 0x10000;
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
    return cp;
}

// Keep what a terminal will display as written: a malformed or stray byte is
// dropped, a control code point is dropped, and everything else is kept.
// `keepLayout` lets tab and newline through. Unlike Lua, which keeps the
// bytes, the result is DECODED text (one code point per character, not one
// code unit per byte): a kept "\u00DB" as the byte string "\xC3\x9B" would
// be UTF-8-encoded again when logged, and its 0x9B byte became a real C1 CSI
// (#1009 review). `maxBytes` bounds the input bytes kept, as Lua's
// `s:sub(1, max)` does, and stops on a whole character; -> [text, truncated].
function stripControls(s, keepLayout, maxBytes) {
    const out = [];
    const n = s.length;
    let i = 0, kept = 0;
    while (i < n) {
        const c = s.charCodeAt(i);
        const len = (c < 0x80 && 1) || (c >= 0xC2 && c <= 0xDF && 2)
            || (c >= 0xE0 && c <= 0xEF && 3) || (c >= 0xF0 && c <= 0xF4 && 4) || 0;
        let cp = -1;
        if (len > 0 && i + len <= n) cp = decodeAt(s, i, len);
        if (cp < 0) {
            i += 1;                                 // malformed: drop this byte
        } else {
            if ((cp >= 0x20 && cp !== 0x7F && !isInvisibleControl(cp))
                || (keepLayout && (cp === 0x09 || cp === 0x0A))) {
                if (maxBytes !== undefined && kept + len > maxBytes) {
                    return [out.join(""), true];
                }
                out.push(String.fromCodePoint(cp));
                kept += len;
            }
            i += len;
        }
    }
    return [out.join(""), false];
}

// Multi-line text: a banner. Tab and newline survive; CR and every other
// control go.
function safeText(s) {
    if (typeof s !== "string") return String(s);
    return stripControls(s, true)[0];
}

// Single-line text: a name or reason interpolated into one line of an error
// message. Tab and newline go too, and the result is bounded (in bytes of the
// kept text, as Lua's).
function safeName(s, max) {
    if (typeof s !== "string") return String(s);
    if (max === undefined || max === null) max = 64;
    const [text, truncated] = stripControls(s, false, max);
    return truncated ? text + "..." : text;
}

// Convenience --------------------------------------------------------

// Encode one length-prefixed string without building a whole writer.
function string(s) {
    return u32be(s.length) + s;
}

// Encode a uint32 on its own.
function uint32(v) {
    return u32be(v >>> 0);
}

// The uint32 at `pos` (default 0) of a raw buffer, without a reader.
function peekUint32(buf, pos) {
    if (pos === undefined || pos === null) pos = 0;
    if (buf.length < pos + 4) {
        throw new Error("ssh.wire: truncated: need 4 byte(s) at " + String(pos)
            + ", have " + String(buf.length - pos));
    }
    return ((buf.charCodeAt(pos) << 24) >>> 0) + (buf.charCodeAt(pos + 1) << 16)
        + (buf.charCodeAt(pos + 2) << 8) + buf.charCodeAt(pos + 3);
}

// Read until `buf` holds at least `n` bytes. `read(max)` resolves to
// [chunk] (short is normal), [""] at end of stream, or [null, err, code];
// `maxChunk` (optional) caps what one call asks for.
//
// Resolves to [buf, true] when it holds n bytes, [buf, false] at end of
// stream, or [buf, null, err, code] when a read failed - the bytes gathered
// so far are kept either way. Pieces are joined once (linear, not quadratic).
async function gather(buf, n, read, maxChunk) {
    let have = buf.length;
    if (have >= n) return [buf, true];
    const parts = [buf];
    while (have < n) {
        let want = n - have;
        if (maxChunk && want > maxChunk) want = maxChunk;
        const [chunk, err, code] = await read(want);
        if (chunk === null || chunk === undefined) return [parts.join(""), null, err, code];
        if (chunk === "") return [parts.join(""), false];
        parts.push(chunk);
        have += chunk.length;
    }
    return [parts.join(""), true];
}

// Zero padding. RANDOM padding is the caller's: no entropy here.
function zeros(n) {
    return "\x00".repeat(n);
}

export const wire = {
    writer, reader, safeText, safeName, string, uint32, peekUint32, gather, zeros,
};
export default wire;
