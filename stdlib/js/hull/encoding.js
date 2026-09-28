/*
 * hull:encoding - byte <-> text codecs: hex, base64 (standard and url-safe),
 * base32, and UTF-8. JS twin of hull.encoding; both produce and accept
 * exactly the same text.
 *
 *   import { encoding } from "hull:encoding";
 *   encoding.hex.encode(bytes)                        // lowercase
 *   encoding.hex.decode(text)                         // byte string | null
 *   encoding.base64.encode(bytes, { url: true })      // -_ alphabet, unpadded
 *   encoding.base64.decode(text, { lenient: true })   // skips whitespace
 *   encoding.base32.encode(bytes)                     // RFC 4648, unpadded
 *   encoding.utf8.encode("héllo")                     // text -> UTF-8 bytes
 *   encoding.utf8.decode(bytes)                       // UTF-8 bytes -> text | null
 *
 * Bytes are a BYTE STRING (one character per byte, 0..255), an ArrayBuffer,
 * a typed array or a DataView; decoders return a byte string. A string with
 * a character above 0xFF is refused rather than truncated, so two different
 * strings can never encode alike. encoding.bytes converts between byte
 * strings and Uint8Array for the C bindings, which take buffers: a JS string
 * handed to C is UTF-8 encoded on the way, which changes every byte >= 0x80.
 *
 * Decoding is strict unless asked otherwise; a decoder returns null on bad
 * input and throws only when handed something that is not a string.
 *
 * Pure: no capabilities and no other module, so anything may import it.
 */

function fail(fname, msg) {
    throw new TypeError("encoding." + fname + ": " + msg);
}

// Every codec below builds its output through one of these two, never with
// `out += piece`. In this QuickJS, `s += x` on a local copies all of `s` each
// time (the string is shared with the variable, so it cannot grow in place),
// which makes a 1 MB value cost a million full copies.
//
// Codes: char codes (0..0xFFFF) buffered and turned into 8 K-code pieces with
// one String.fromCharCode each - also small enough that apply() cannot
// overflow the stack. The pieces are joined once at the end.
function Codes() {
    this.buf = [];
    this.parts = [];
}
Codes.prototype.push = function (c) {
    this.buf.push(c);
    if (this.buf.length === 8192) {
        this.parts.push(String.fromCharCode.apply(null, this.buf));
        this.buf = [];
    }
};
Codes.prototype.done = function () {
    if (this.buf.length) this.parts.push(String.fromCharCode.apply(null, this.buf));
    return this.parts.join("");
};

// Any accepted byte input as a byte string.
function toByteString(fname, x) {
    if (typeof x === "string") {
        for (let i = 0; i < x.length; i++) {
            if (x.charCodeAt(i) > 0xff) fail(fname, "string has a character above 0xFF (not bytes)");
        }
        return x;
    }
    let u8;
    if (x instanceof ArrayBuffer) u8 = new Uint8Array(x);
    else if (ArrayBuffer.isView(x)) u8 = new Uint8Array(x.buffer, x.byteOffset, x.byteLength);
    else fail(fname, "expected a byte string, ArrayBuffer or typed array");
    const parts = [];
    for (let i = 0; i < u8.length; i += 8192) {
        parts.push(String.fromCharCode.apply(null, u8.subarray(i, i + 8192)));
    }
    return parts.join("");
}

function checkString(fname, s) {
    if (typeof s !== "string") fail(fname, "expected a string, got " + typeof s);
}

const isWs = (c) => c === 32 || c === 9 || c === 10 || c === 13;

// Bytes ---------------------------------------------------------------------

const bytes = {
    /** A byte string (or any buffer) as a Uint8Array, for the C bindings. */
    toU8(x) {
        const s = toByteString("bytes.toU8", x);
        const u8 = new Uint8Array(s.length);
        for (let i = 0; i < s.length; i++) u8[i] = s.charCodeAt(i);
        return u8;
    },
    /** A buffer (ArrayBuffer / typed array) as a byte string. */
    fromBuffer(x) {
        if (typeof x === "string") fail("bytes.fromBuffer", "expected a buffer, got a string");
        return toByteString("bytes.fromBuffer", x);
    },
};

// Hex -----------------------------------------------------------------------

const HEX_OUT = [];
for (let i = 0; i < 256; i++) HEX_OUT[i] = (i < 16 ? "0" : "") + i.toString(16);

function hexVal(c) {
    if (c >= 48 && c <= 57) return c - 48;
    if (c >= 97 && c <= 102) return c - 87;
    if (c >= 65 && c <= 70) return c - 55;
    return -1;
}

const hex = {
    /** Lowercase hex, two characters per byte. */
    encode(x) {
        const s = toByteString("hex.encode", x);
        const parts = new Array(s.length);
        for (let i = 0; i < s.length; i++) parts[i] = HEX_OUT[s.charCodeAt(i)];
        return parts.join("");
    },
    /** Bytes from hex; either case is accepted. */
    decode(text) {
        checkString("hex.decode", text);
        if (text.length % 2 !== 0) return null;
        const out = new Codes();
        for (let i = 0; i < text.length; i += 2) {
            const hi = hexVal(text.charCodeAt(i)), lo = hexVal(text.charCodeAt(i + 1));
            if (hi < 0 || lo < 0) return null;
            out.push(hi * 16 + lo);
        }
        return out.done();
    },
};

// Base64 --------------------------------------------------------------------

const STD = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

function decodeTable(alphabet) {
    const d = new Int16Array(256).fill(-1);
    for (let i = 0; i < alphabet.length; i++) d[alphabet.charCodeAt(i)] = i;
    return d;
}
const STD_DEC = decodeTable(STD);
const URL_DEC = decodeTable(URL);

const base64 = {
    /**
     * Base64 of `x`. opts.url selects the url-safe alphabet (-_). Padding
     * defaults to on for the standard alphabet and off for the url-safe one;
     * opts.pad overrides.
     */
    encode(x, opts) {
        const s = toByteString("base64.encode", x);
        const url = !!(opts && opts.url);
        let pad = opts && opts.pad;
        if (pad === undefined || pad === null) pad = !url;
        const E = url ? URL : STD;
        const parts = [];
        let i = 0;
        for (; i + 2 < s.length; i += 3) {
            const v = (s.charCodeAt(i) << 16) | (s.charCodeAt(i + 1) << 8) | s.charCodeAt(i + 2);
            parts.push(E[v >> 18] + E[(v >> 12) & 63] + E[(v >> 6) & 63] + E[v & 63]);
        }
        const rem = s.length - i;
        if (rem === 1) {
            const v = s.charCodeAt(i) << 4;
            parts.push(E[v >> 6] + E[v & 63] + (pad ? "==" : ""));
        } else if (rem === 2) {
            const v = (s.charCodeAt(i) << 10) | (s.charCodeAt(i + 1) << 2);
            parts.push(E[v >> 12] + E[(v >> 6) & 63] + E[v & 63] + (pad ? "=" : ""));
        }
        return parts.join("");
    },

    /**
     * Bytes from base64. opts.url selects the url-safe alphabet, which takes
     * no padding; the standard alphabet takes padding or none, but if present
     * it must be exactly right. The unused low bits of the last character
     * must be zero, so each value has exactly one encoding. opts.lenient
     * skips whitespace.
     */
    decode(text, opts) {
        checkString("base64.decode", text);
        const url = !!(opts && opts.url);
        const lenient = !!(opts && opts.lenient);
        const D = url ? URL_DEC : STD_DEC;
        const out = new Codes();
        let acc = 0, bits = 0, count = 0, padding = 0;
        for (let i = 0; i < text.length; i++) {
            const c = text.charCodeAt(i);
            const v = c < 256 ? D[c] : -1;
            if (v >= 0 && padding === 0) {
                acc = ((acc << 6) | v) & 0xffffff;
                bits += 6;
                count++;
                if (bits >= 8) {
                    bits -= 8;
                    out.push((acc >> bits) & 0xff);
                }
            } else if (c === 61 && !url) {
                padding++;
            } else if (!(lenient && isWs(c))) {
                return null;
            }
        }
        if (count % 4 === 1) return null;
        if (padding > 0 && (padding > 2 || (count + padding) % 4 !== 0)) return null;
        if ((acc & ((1 << bits) - 1)) !== 0) return null;      // non-canonical
        return out.done();
    },
};

// Base32 --------------------------------------------------------------------

const B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
const B32_DEC = decodeTable(B32);
for (let i = 0; i < 26; i++) B32_DEC[B32.charCodeAt(i) + 32] = i;   // lowercase too

// Base32 lengths (mod 8) that some byte count encodes to: 0, 2, 4, 5 and 7.
const B32_LEN_OK = [true, false, true, false, true, true, false, true];

const base32 = {
    /** RFC 4648 base32, uppercase, unpadded (the form authenticator apps take). */
    encode(x) {
        const s = toByteString("base32.encode", x);
        const parts = [];
        let buf = 0, bits = 0;
        for (let i = 0; i < s.length; i++) {
            buf = ((buf << 8) | s.charCodeAt(i)) & 0xffff;
            bits += 8;
            while (bits >= 5) {
                bits -= 5;
                parts.push(B32[(buf >> bits) & 31]);
            }
        }
        if (bits > 0) parts.push(B32[(buf << (5 - bits)) & 31]);
        return parts.join("");
    },

    /**
     * Bytes from base32, either case. Strictly, only the alphabet, a length
     * some input encodes to, and zero unused low bits; opts.lenient also
     * skips whitespace and '='.
     */
    decode(text, opts) {
        checkString("base32.decode", text);
        const lenient = !!(opts && opts.lenient);
        const out = new Codes();
        let buf = 0, bits = 0, count = 0;
        for (let i = 0; i < text.length; i++) {
            const c = text.charCodeAt(i);
            const v = c < 256 ? B32_DEC[c] : -1;
            if (v >= 0) {
                buf = ((buf << 5) | v) & 0xffff;
                bits += 5;
                count++;
                if (bits >= 8) {
                    bits -= 8;
                    out.push((buf >> bits) & 0xff);
                }
            } else if (!(lenient && (isWs(c) || c === 61))) {
                return null;
            }
        }
        if (!B32_LEN_OK[count % 8]) return null;
        if ((buf & ((1 << bits) - 1)) !== 0) return null;      // non-canonical
        return out.done();
    },
};

// UTF-8 ---------------------------------------------------------------------
//
// A JS string is text (UTF-16); the codecs above work on bytes. These are the
// two crossings: text to its UTF-8 bytes (before hashing or base64-encoding a
// JSON payload) and back. Strict both ways, so Lua and JS accept exactly the
// same input: a lone surrogate cannot be encoded, and malformed UTF-8
// (overlong forms, surrogates, anything above U+10FFFF, truncation) does not
// decode.

const utf8 = {
    /** The UTF-8 bytes of `text`, as a byte string. Throws on a lone surrogate. */
    encode(text) {
        checkString("utf8.encode", text);
        const out = new Codes();
        for (let i = 0; i < text.length; i++) {
            let c = text.charCodeAt(i);
            if (c < 0x80) { out.push(c); continue; }
            if (c >= 0xd800 && c <= 0xdfff) {
                const d = i + 1 < text.length ? text.charCodeAt(i + 1) : 0;
                if (c > 0xdbff || d < 0xdc00 || d > 0xdfff) {
                    fail("utf8.encode", "lone surrogate at index " + i);
                }
                c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
                i++;
            }
            if (c < 0x800) {
                out.push(0xc0 | (c >> 6)); out.push(0x80 | (c & 63));
            } else if (c < 0x10000) {
                out.push(0xe0 | (c >> 12)); out.push(0x80 | ((c >> 6) & 63));
                out.push(0x80 | (c & 63));
            } else {
                out.push(0xf0 | (c >> 18)); out.push(0x80 | ((c >> 12) & 63));
                out.push(0x80 | ((c >> 6) & 63)); out.push(0x80 | (c & 63));
            }
        }
        return out.done();
    },

    /** `bytes` (byte string or buffer) as text, or null if not well-formed UTF-8. */
    decode(x) {
        const s = toByteString("utf8.decode", x);
        const out = new Codes();
        for (let i = 0; i < s.length;) {
            const b = s.charCodeAt(i);
            if (b < 0x80) { out.push(b); i++; continue; }
            let n, c, min;
            if (b >= 0xc2 && b <= 0xdf) { n = 1; c = b & 0x1f; min = 0x80; }
            else if (b >= 0xe0 && b <= 0xef) { n = 2; c = b & 0x0f; min = 0x800; }
            else if (b >= 0xf0 && b <= 0xf4) { n = 3; c = b & 0x07; min = 0x10000; }
            else return null;
            for (let k = 1; k <= n; k++) {     // a missing byte reads as -1: truncated
                const t = i + k < s.length ? s.charCodeAt(i + k) : -1;
                if (t < 0x80 || t > 0xbf) return null;
                c = (c << 6) | (t & 0x3f);
            }
            if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return null;
            if (c < 0x10000) {
                out.push(c);
            } else {                           // as a UTF-16 surrogate pair
                c -= 0x10000;
                out.push(0xd800 | (c >> 10)); out.push(0xdc00 | (c & 0x3ff));
            }
            i += n + 1;
        }
        return out.done();
    },
};

export const encoding = { hex, base64, base32, utf8, bytes };
export default encoding;
