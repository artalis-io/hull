/*
 * hull:encoding - byte <-> text codecs: hex, base64 (standard and url-safe),
 * base32. JS twin of hull.encoding; both produce and accept exactly the same
 * text.
 *
 *   import { encoding } from "hull:encoding";
 *   encoding.hex.encode(bytes)                        // lowercase
 *   encoding.hex.decode(text)                         // byte string | null
 *   encoding.base64.encode(bytes, { url: true })      // -_ alphabet, unpadded
 *   encoding.base64.decode(text, { lenient: true })   // skips whitespace
 *   encoding.base32.encode(bytes)                     // RFC 4648, unpadded
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
    // Chunked: one apply per 8 KiB rather than per byte, and small enough
    // that a large value cannot overflow the call stack.
    let s = "";
    for (let i = 0; i < u8.length; i += 8192) {
        s += String.fromCharCode.apply(null, u8.subarray(i, i + 8192));
    }
    return s;
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
        let out = "";
        for (let i = 0; i < s.length; i++) out += HEX_OUT[s.charCodeAt(i)];
        return out;
    },
    /** Bytes from hex; either case is accepted. */
    decode(text) {
        checkString("hex.decode", text);
        if (text.length % 2 !== 0) return null;
        let out = "";
        for (let i = 0; i < text.length; i += 2) {
            const hi = hexVal(text.charCodeAt(i)), lo = hexVal(text.charCodeAt(i + 1));
            if (hi < 0 || lo < 0) return null;
            out += String.fromCharCode(hi * 16 + lo);
        }
        return out;
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
        let out = "";
        let i = 0;
        for (; i + 2 < s.length; i += 3) {
            const v = (s.charCodeAt(i) << 16) | (s.charCodeAt(i + 1) << 8) | s.charCodeAt(i + 2);
            out += E[v >> 18] + E[(v >> 12) & 63] + E[(v >> 6) & 63] + E[v & 63];
        }
        const rem = s.length - i;
        if (rem === 1) {
            const v = s.charCodeAt(i) << 4;
            out += E[v >> 6] + E[v & 63] + (pad ? "==" : "");
        } else if (rem === 2) {
            const v = (s.charCodeAt(i) << 10) | (s.charCodeAt(i + 1) << 2);
            out += E[v >> 12] + E[(v >> 6) & 63] + E[v & 63] + (pad ? "=" : "");
        }
        return out;
    },

    /**
     * Bytes from base64. opts.url selects the url-safe alphabet, which takes
     * no padding; the standard alphabet takes padding or none, but if present
     * it must be exactly right. opts.lenient skips whitespace.
     */
    decode(text, opts) {
        checkString("base64.decode", text);
        const url = !!(opts && opts.url);
        const lenient = !!(opts && opts.lenient);
        const D = url ? URL_DEC : STD_DEC;
        let out = "";
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
                    out += String.fromCharCode((acc >> bits) & 0xff);
                }
            } else if (c === 61 && !url) {
                padding++;
            } else if (!(lenient && isWs(c))) {
                return null;
            }
        }
        if (count % 4 === 1) return null;
        if (padding > 0 && (padding > 2 || (count + padding) % 4 !== 0)) return null;
        return out;
    },
};

// Base32 --------------------------------------------------------------------

const B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
const B32_DEC = decodeTable(B32);
for (let i = 0; i < 26; i++) B32_DEC[B32.charCodeAt(i) + 32] = i;   // lowercase too

const base32 = {
    /** RFC 4648 base32, uppercase, unpadded (the form authenticator apps take). */
    encode(x) {
        const s = toByteString("base32.encode", x);
        let out = "";
        let buf = 0, bits = 0;
        for (let i = 0; i < s.length; i++) {
            buf = ((buf << 8) | s.charCodeAt(i)) & 0xffff;
            bits += 8;
            while (bits >= 5) {
                bits -= 5;
                out += B32[(buf >> bits) & 31];
            }
        }
        if (bits > 0) out += B32[(buf << (5 - bits)) & 31];
        return out;
    },

    /**
     * Bytes from base32, either case. Strictly, only the alphabet;
     * opts.lenient also skips whitespace and '='.
     */
    decode(text, opts) {
        checkString("base32.decode", text);
        const lenient = !!(opts && opts.lenient);
        let out = "";
        let buf = 0, bits = 0;
        for (let i = 0; i < text.length; i++) {
            const c = text.charCodeAt(i);
            const v = c < 256 ? B32_DEC[c] : -1;
            if (v >= 0) {
                buf = ((buf << 5) | v) & 0xffff;
                bits += 5;
                if (bits >= 8) {
                    bits -= 8;
                    out += String.fromCharCode((buf >> bits) & 0xff);
                }
            } else if (!(lenient && (isWs(c) || c === 61))) {
                return null;
            }
        }
        return out;
    },
};

export const encoding = { hex, base64, base32, bytes };
export default encoding;
