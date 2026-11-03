// hull:ssh:cipher - the aes256-gcm@openssh.com packet layer.
//
// The JS twin of hull.ssh.cipher (stdlib/lua/hull/ssh/cipher.lua): RFC 5647 as
// OpenSSH implements it. The invocation counter lives inside this object,
// advances exactly once per packet, and no caller can supply an IV.
//
//   uint32   packet_length      plaintext, authenticated as AAD
//   byte     padding_length  \
//   byte[]   payload          |  encrypted
//   byte[]   padding         /
//   byte[16] tag
//
// The AEAD is injected: { seal(key, iv, aad, pt) -> [ct, tag],
// open(key, iv, aad, ct, tag) -> pt | null }.

import { packet } from "hull:ssh:packet";
import { wire } from "hull:ssh:wire";

const KEY_LEN = 32;
const IV_LEN = 12;
const TAG_LEN = 16;
const BLOCK = 16;       // the encrypted region aligns to the AES block
const LENGTH_LEN = 4;

// A backstop that should never be reached (see the Lua twin).
const MAX_PACKETS = 0x80000000;

// When a rekey is DUE: 1 GiB per direction, or 2^27 packets. No time trigger.
const REKEY_BYTES = 1024 * 1024 * 1024;
const REKEY_PACKETS = 0x08000000;

// Increment the 8-byte big-endian invocation counter.
function bump(counter) {
    const b = [];
    for (let i = 0; i < 8; i++) b.push(counter.charCodeAt(i));
    for (let i = 7; i >= 0; i--) {
        b[i] += 1;
        if (b[i] <= 255) break;
        b[i] = 0;
        if (i === 0) {
            throw new Error("ssh.cipher: invocation counter wrapped; the key must not be reused");
        }
    }
    return String.fromCharCode.apply(null, b);
}

// One direction of the connection.
class Cipher {
    constructor(key, iv) {
        this.key = key;
        this.fixed = iv.slice(0, 4);
        this.counter = iv.slice(4, 12);
        this.packets = 0;
        this.bytes = 0;
    }

    iv() {
        return this.fixed + this.counter;
    }

    // Advance to the next packet. Called on BOTH seal and open.
    advance() {
        this.counter = bump(this.counter);
        this.packets += 1;
        if (this.packets >= MAX_PACKETS) {
            throw new Error("ssh.cipher: " + String(this.packets)
                + " packets under one key without a rekey; refusing to continue");
        }
    }

    packetsSent() {
        return this.packets;
    }

    // Bytes this key has protected, whole frames.
    bytesProcessed() {
        return this.bytes;
    }

    // Whether a rekey is DUE. Advisory.
    rekeyDue(limits) {
        limits = limits || {};
        const maxBytes = limits.bytes || REKEY_BYTES;
        const maxPackets = limits.packets || REKEY_PACKETS;
        return this.bytes >= maxBytes || this.packets >= maxPackets;
    }

    // Encrypt one payload into a wire packet.
    seal(aead, payload, randomBytes) {
        if (typeof randomBytes !== "function") {
            throw new TypeError("ssh.cipher: a random_bytes function is required");
        }
        // The length field is NOT part of the aligned region for an AEAD.
        const pad = packet.paddingFor(payload.length, BLOCK, false);
        const padding = randomBytes(pad);
        if (padding.length !== pad) {
            throw new Error("ssh.cipher: random_bytes returned the wrong length");
        }
        const plain = String.fromCharCode(pad) + payload + padding;
        if (plain.length % BLOCK !== 0) {
            throw new Error("ssh.cipher: encrypted region is not block aligned");
        }
        const aad = wire.uint32(plain.length);

        const r = aead.seal(this.key, this.iv(), aad, plain);
        const ct = r && r[0], tag = r && r[1];
        if (typeof ct !== "string" || ct.length !== plain.length
            || typeof tag !== "string" || tag.length !== TAG_LEN) {
            throw new Error("ssh.cipher: AEAD seal returned the wrong shape");
        }
        this.advance();
        const fr = aad + ct + tag;
        this.bytes += fr.length;
        return fr;
    }

    // The whole frame's size, from the front of `buf` (below 4 bytes, 4).
    needed(buf) {
        if (buf.length < LENGTH_LEN) return LENGTH_LEN;
        const packetLength = wire.peekUint32(buf);
        checkLength(packetLength);
        return frameSize(packetLength);
    }

    // Decrypt one packet from the front of `buf`.
    // -> [payload, consumed] | [null, "need_more"]; throws on a length that
    // cannot be valid or an authentication failure (not recoverable).
    open(aead, buf) {
        if (buf.length < LENGTH_LEN) return [null, "need_more"];

        const packetLength = wire.peekUint32(buf);
        checkLength(packetLength);

        const total = frameSize(packetLength);
        if (buf.length < total) return [null, "need_more"];

        const aad = buf.slice(0, LENGTH_LEN);
        const ct = buf.slice(LENGTH_LEN, LENGTH_LEN + packetLength);
        const tag = buf.slice(LENGTH_LEN + packetLength, total);

        const plain = aead.open(this.key, this.iv(), aad, ct, tag);
        if (plain === null || plain === undefined || plain === false) {
            throw new Error("ssh.cipher: packet failed authentication");
        }
        this.advance();
        this.bytes += total;

        const pad = plain.charCodeAt(0);
        if (pad < packet.MIN_PADDING || pad + 1 > plain.length) {
            throw new Error("ssh.cipher: padding of " + String(pad) + " is not usable");
        }
        return [plain.slice(1, plain.length - pad), total];
    }
}

function newCipher(key, iv) {
    if (typeof key !== "string" || key.length !== KEY_LEN) {
        throw new TypeError("ssh.cipher: key must be 32 bytes");
    }
    if (typeof iv !== "string" || iv.length !== IV_LEN) {
        throw new TypeError("ssh.cipher: iv must be 12 bytes");
    }
    return new Cipher(key, iv);
}

// How many bytes a full packet occupies, given its plaintext length field.
function frameSize(packetLength) {
    return LENGTH_LEN + packetLength + TAG_LEN;
}

// The declared packet length, refused unless plausible.
function checkLength(packetLength) {
    if (packetLength > packet.MAX_PACKET) {
        throw new Error("ssh.cipher: declared packet length " + String(packetLength)
            + " exceeds the maximum");
    }
    if (packetLength < BLOCK || packetLength % BLOCK !== 0) {
        throw new Error("ssh.cipher: declared packet length " + String(packetLength)
            + " is not a whole number of blocks");
    }
}

export const cipher = {
    KEY_LEN, IV_LEN, TAG_LEN, BLOCK, LENGTH_LEN, MAX_PACKETS, REKEY_BYTES, REKEY_PACKETS,
    new: newCipher, frameSize,
};
export default cipher;
