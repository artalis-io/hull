// hull:ssh:chacha - the chacha20-poly1305@openssh.com packet layer.
//
// The JS twin of hull.ssh.chacha (stdlib/lua/hull/ssh/chacha.lua): OpenSSH's
// construction (PROTOCOL.chacha20poly1305), not the IETF AEAD. The SECOND half
// of the 64-byte key (K_1) encrypts only the length, the FIRST (K_2)
// everything else; the nonce is the packet's sequence number.
//
// The primitives are injected: `prims` = { chacha20(key, nonce12, counter,
// data), poly1305(key, msg), ctEq(a, b) } (Lua: ct_eq) - hull:ssh:bytes'
// crypto.chacha20 / poly1305 / constantTimeEq.

import { packet } from "hull:ssh:packet";
import { cipher } from "hull:ssh:cipher";     // the shared per-key limits
import { wire } from "hull:ssh:wire";

const NAME = "chacha20-poly1305@openssh.com";
const KEY_LEN = 64;
const TAG_LEN = 16;
const BLOCK = 8;
const LENGTH_LEN = 4;

const ZEROS32 = "\x00".repeat(32);

// RFC 8439's 12-byte nonce carrying OpenSSH's 64-bit one.
function nonce(seq) {
    if (!Number.isInteger(seq) || seq < 0 || seq > 0xFFFFFFFF) {
        throw new Error("ssh.chacha: a sequence number is required");
    }
    return "\x00\x00\x00\x00" + "\x00\x00\x00\x00" + wire.uint32(seq);
}

class Chacha {
    constructor(key, prims) {
        this.kMain = key.slice(0, 32);
        this.kLen = key.slice(32, 64);
        this.prims = prims;
        this.packets = 0;
        this.bytes = 0;
    }

    // Advance to the next packet; the per-key limits are hull:ssh:cipher's.
    advance() {
        this.packets += 1;
        if (this.packets >= cipher.MAX_PACKETS) {
            throw new Error("ssh.chacha: " + String(this.packets)
                + " packets under one key without a rekey; refusing to continue");
        }
    }

    packetsSent() { return this.packets; }
    bytesProcessed() { return this.bytes; }

    rekeyDue(limits) {
        limits = limits || {};
        return this.bytes >= (limits.bytes || cipher.REKEY_BYTES)
            || this.packets >= (limits.packets || cipher.REKEY_PACKETS);
    }

    // Encrypt one payload into a wire packet. `_aead` is unused here.
    seal(_aead, payload, randomBytes, seq) {
        if (typeof randomBytes !== "function") {
            throw new TypeError("ssh.chacha: a random_bytes function is required");
        }
        const n = nonce(seq);
        const pad = packet.paddingFor(payload.length, BLOCK, false);
        const padding = randomBytes(pad);
        if (padding.length !== pad) {
            throw new Error("ssh.chacha: random_bytes returned the wrong length");
        }
        const plain = String.fromCharCode(pad) + payload + padding;
        if (plain.length + LENGTH_LEN > packet.MAX_PACKET) {
            throw new RangeError("ssh.chacha: packet too large");
        }

        const p = this.prims;
        const encLen = p.chacha20(this.kLen, n, 0, wire.uint32(plain.length));
        const polyKey = p.chacha20(this.kMain, n, 0, ZEROS32);
        const encBody = p.chacha20(this.kMain, n, 1, plain);
        const tag = p.poly1305(polyKey, encLen + encBody);
        if (encLen.length !== 4 || encBody.length !== plain.length || tag.length !== TAG_LEN) {
            throw new Error("ssh.chacha: a primitive returned the wrong shape");
        }
        this.advance();
        const fr = encLen + encBody + tag;
        this.bytes += fr.length;
        return fr;
    }

    // The whole frame's size, from the (encrypted, unauthenticated) length at
    // the front of `buf` - bounded on plausibility first. Below 4 bytes, 4.
    needed(buf, seq) {
        if (buf.length < LENGTH_LEN) return LENGTH_LEN;
        const len = this.prims.chacha20(this.kLen, nonce(seq), 0, buf.slice(0, LENGTH_LEN));
        const packetLength = wire.peekUint32(len);
        if (packetLength + LENGTH_LEN > packet.MAX_PACKET) {
            throw new Error("ssh.chacha: declared packet length " + String(packetLength)
                + " exceeds the maximum");
        }
        if (packetLength < BLOCK || packetLength % BLOCK !== 0) {
            throw new Error("ssh.chacha: declared packet length " + String(packetLength)
                + " is not a whole number of blocks");
        }
        return LENGTH_LEN + packetLength + TAG_LEN;
    }

    // Decrypt one packet: [payload, consumed] | [null, "need_more"]. Throws
    // on authentication failure (the end of the connection).
    open(_aead, buf, seq) {
        if (buf.length < LENGTH_LEN) return [null, "need_more"];
        const total = this.needed(buf, seq);
        if (buf.length < total) return [null, "need_more"];

        const n = nonce(seq);
        const p = this.prims;
        const bodyEnd = total - TAG_LEN;
        const polyKey = p.chacha20(this.kMain, n, 0, ZEROS32);
        const want = p.poly1305(polyKey, buf.slice(0, bodyEnd));
        // Authenticate before decrypting anything past the length.
        if (!p.ctEq(want, buf.slice(bodyEnd, total))) {
            throw new Error("ssh.chacha: packet failed authentication");
        }
        const plain = p.chacha20(this.kMain, n, 1, buf.slice(LENGTH_LEN, bodyEnd));
        this.advance();
        this.bytes += total;

        const pad = plain.charCodeAt(0);
        if (pad < packet.MIN_PADDING || pad + 1 > plain.length) {
            throw new Error("ssh.chacha: padding of " + String(pad) + " is not usable");
        }
        return [plain.slice(1, plain.length - pad), total];
    }
}

// One direction of the connection.
function newChacha(key, prims) {
    if (typeof key !== "string" || key.length !== KEY_LEN) {
        throw new TypeError("ssh.chacha: key must be 64 bytes");
    }
    if (prims === null || typeof prims !== "object" || typeof prims.chacha20 !== "function"
        || typeof prims.poly1305 !== "function" || typeof prims.ctEq !== "function") {
        throw new TypeError("ssh.chacha: chacha20, poly1305 and ct_eq are required");
    }
    return new Chacha(key, prims);
}

export const chacha = { NAME, KEY_LEN, TAG_LEN, BLOCK, LENGTH_LEN, new: newChacha };
export default chacha;
