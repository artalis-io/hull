// hull:ssh:packet - version exchange and the binary packet protocol.
//
// The JS twin of hull.ssh.packet (stdlib/lua/hull/ssh/packet.lua): RFC 4253
// section 4.2 (identification string) and section 6 (binary packet protocol).
// Pure functions over byte strings: nothing here does I/O, holds a key, or
// knows a cipher. Framing is where a peer first gets to choose a number that
// Hull then allocates against, so the length checks are the load-bearing part.

import { wire } from "hull:ssh:wire";

// RFC 4253 section 6.1: a ceiling rather than a target.
const MAX_PACKET = 35000;
const MIN_PADDING = 4;          // RFC 4253 section 6
const MIN_BLOCK = 8;            // padding aligns to max(8, cipher block size)

// RFC 4253 section 4.2: at most 255 bytes including the trailing CR LF.
const MAX_IDENT = 255;

// Build our identification line. `software` is letters, digits, dot and
// underscore only (the field is delimited by space and minus).
function buildIdent(software) {
    if (typeof software !== "string" || software === "") {
        throw new TypeError("ssh.packet: software version must be a non-empty string");
    }
    if (/[^A-Za-z0-9._]/.test(software)) {
        throw new TypeError("ssh.packet: software version may use only letters, digits, dot and underscore");
    }
    const line = "SSH-2.0-" + software + "\r\n";
    if (line.length > MAX_IDENT) {
        throw new RangeError("ssh.packet: identification string too long");
    }
    return line;
}

// Parse one identification line, WITHOUT its CR LF.
// -> [ { protoversion, software, comments, line } ] | [null, reason]
function parseIdent(line) {
    if (typeof line !== "string") return [null, "not a string"];
    if (line.length > MAX_IDENT) return [null, "identification string too long"];
    const m = /^SSH-([^\r\n]*)$/.exec(line);
    if (!m) return [null, "not an identification string"];
    const rest = m[1];
    // protoversion "-" softwareversion [SP comments]
    const pm = /^([^-]+)-([\s\S]*)$/.exec(rest);
    if (!pm || pm[2] === "") return [null, "malformed identification string"];
    const proto = pm[1], tail = pm[2];
    if (proto !== "2.0") {
        // 1.99 means a server willing to speak either; Hull speaks only 2.0.
        return [null, "unsupported protocol version: " + proto];
    }
    let software, comments = null;
    const sm = /^([^ ]+) ([\s\S]*)$/.exec(tail);
    if (sm) { software = sm[1]; comments = sm[2]; } else { software = tail; }
    return [{ protoversion: proto, software, comments, line }];
}

// How much padding a payload needs. With an AEAD the length travels as
// associated data and is NOT counted, hence `lengthInBlock`.
function paddingFor(payloadLen, block, lengthInBlock) {
    let b = block || MIN_BLOCK;
    if (b < MIN_BLOCK) b = MIN_BLOCK;
    const base = payloadLen + 1 + (lengthInBlock ? 4 : 0);
    let pad = b - (base % b);
    if (pad < MIN_PADDING) pad += b;
    return pad;
}

// Frame a payload into an unencrypted packet. `randomBytes(n)` supplies the
// padding: this module has no capability access.
function frame(payload, block, randomBytes) {
    if (typeof payload !== "string") {
        throw new TypeError("ssh.packet: payload must be a string");
    }
    if (typeof randomBytes !== "function") {
        throw new TypeError("ssh.packet: a random_bytes function is required");
    }
    const pad = paddingFor(payload.length, block, true);
    const packetLength = payload.length + pad + 1;
    if (packetLength + 4 > MAX_PACKET) {
        throw new RangeError("ssh.packet: packet too large");
    }
    const padding = randomBytes(pad);
    if (typeof padding !== "string" || padding.length !== pad) {
        throw new Error("ssh.packet: random_bytes returned the wrong length");
    }
    return wire.uint32(packetLength) + String.fromCharCode(pad) + payload + padding;
}

// Parse one unencrypted packet from the front of `buf`.
// -> [payload, bytesConsumed] | [null, "need_more"]; throws on a packet that
// cannot be valid whatever arrives next.
function parse(buf, block) {
    if (typeof buf !== "string") {
        throw new TypeError("ssh.packet: buf must be a string");
    }
    if (buf.length < 4) return [null, "need_more"];

    const packetLength = wire.peekUint32(buf);

    // Bound the claim BEFORE waiting for that many bytes.
    if (packetLength + 4 > MAX_PACKET) {
        throw new Error("ssh.packet: declared packet length " + String(packetLength)
            + " exceeds the maximum");
    }
    if (packetLength < 1 + MIN_PADDING) {
        throw new Error("ssh.packet: declared packet length " + String(packetLength)
            + " is below the minimum");
    }
    let b = block || MIN_BLOCK;
    if (b < MIN_BLOCK) b = MIN_BLOCK;
    if ((packetLength + 4) % b !== 0) {
        throw new Error("ssh.packet: packet length is not a multiple of the block size");
    }

    if (buf.length < packetLength + 4) return [null, "need_more"];

    const paddingLength = buf.charCodeAt(4);
    if (paddingLength < MIN_PADDING) {
        throw new Error("ssh.packet: padding of " + String(paddingLength)
            + " is below the minimum");
    }
    const payloadLen = packetLength - paddingLength - 1;
    if (payloadLen < 0) {
        throw new Error("ssh.packet: padding exceeds the packet");
    }
    return [buf.slice(5, 5 + payloadLen), packetLength + 4];
}

// Zero padding, for tests and for callers that do their own randomness.
function zeroPadding(n) {
    return "\x00".repeat(n);
}

export const packet = {
    MAX_PACKET, MIN_PADDING, MIN_BLOCK, MAX_IDENT,
    buildIdent, parseIdent, paddingFor, frame, parse, zeroPadding,
};
export default packet;
