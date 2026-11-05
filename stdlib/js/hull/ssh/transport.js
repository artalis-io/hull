// hull:ssh:transport - the connection: handshake, encrypted packets, channels.
//
// The JS twin of hull.ssh.transport (stdlib/lua/hull/ssh/transport.lua); see
// there for the reasoning behind each rule. It owns the sequence
// (identification, KEXINIT, key exchange, NEWKEYS, userauth) and the packet
// loop that runs afterwards.
//
// The stream and the crypto are BOTH injected. In production they are the
// private byte stream (hull:ssh:_io) and hull:ssh:bytes' crypto; in tests a
// buffer and a stub. The stream interface (duck-typed):
//
//   await stream.read(n)   -> [bytes (1..n)] | [""] at EOF | [null, err, code]
//   await stream.write(s)  -> [true] | [null, err, code]
//   stream.close()
//
// read() returning SHORT is normal. Every method that reaches the stream is
// async; where Lua's coroutines interleave at yields, two JS callers
// interleave at awaits, and the same io_busy flag refuses the second.
//
// One difference the port has to make up for: a Lua coroutine resumed from a
// stream call runs straight on to its next statement, but a JS caller resumes
// through a chain of awaits, and other promise jobs run in between - after
// streamCall has cleared ioBusy and before the caller has finished its
// bookkeeping. So a packet's whole send (seal, sequence number, write) and a
// packet's whole read (buffer, sequence number, keys) each hold a lock of
// their own (sendBusy / readBusy) for their duration, and the state they
// advance is advanced before the first await (audit of #1009: a second task
// sealed with the sequence number the first had just used - the
// chacha20-poly1305 nonce).

import { packet } from "hull:ssh:packet";
import { kexinit } from "hull:ssh:kexinit";
import { kex } from "hull:ssh:kex";
import { hostkey } from "hull:ssh:hostkey";
import { cipher } from "hull:ssh:cipher";
import { chacha } from "hull:ssh:chacha";
import { userauth } from "hull:ssh:userauth";
import { channel } from "hull:ssh:channel";
import { session } from "hull:ssh:session";
import { wire } from "hull:ssh:wire";
import { sftpClient } from "hull:ssh:sftp_client";

// The most channel data held back while a rekey WE started waits for the
// peer's KEXINIT (see deferMessage).
const DEFER_MAX_BYTES = 8 * 1024 * 1024;

// Transport-layer messages that can arrive at any time (RFC 4253 section 11).
const SSH_MSG_DISCONNECT = 1;
const SSH_MSG_IGNORE = 2;
const SSH_MSG_UNIMPLEMENTED = 3;
const SSH_MSG_DEBUG = 4;
const SSH_MSG_GLOBAL_REQUEST = 80;
const SSH_MSG_REQUEST_SUCCESS = 81;
const SSH_MSG_REQUEST_FAILURE = 82;
const SSH_MSG_KEXINIT = 20;

// Liveness defaults (see Transport.quiet).
const KEEPALIVE_MS = 30000;
const KEEPALIVE_MAX = 3;
const IDLE_MS = 60000;

function sshError(code, detail, extra) {
    const e = new Error(detail === undefined || detail === null ? code : String(detail));
    e.code = code;
    if (detail !== undefined) e.detail = detail;
    if (extra) for (const k of Object.keys(extra)) e[k] = extra[k];
    // The connection's own failure, never a runtime refusal (hull:_refusal):
    // its "busy" means this connection is in use, not an admission cap.
    Object.defineProperty(e, "ssh", { value: true, enumerable: false });
    return e;
}

// Failures the layers above branch on carry a code:
//   "deadline"  a phase ran out of time; the connection itself may be fine
//   "timeout"   the server stopped answering; the connection is dead
function fail(code, detail) {
    throw sshError(code, detail);
}

const errText = (e) => (e && typeof e === "object" && typeof e.message === "string")
    ? e.message : String(e);

const KEEPALIVE = wire.writer().byte(80).string("keepalive@openssh.com")
    .boolean(true).build();

// The most asked of the stream in one read: never a number the peer chose.
const READ_CHUNK = 32768;

class Transport {
    // How long any one read or write may wait: the keepalive interval when
    // keepalives are on, else the idle bound, else unbounded.
    applyWait() {
        const w = (this.keepaliveMs > 0 && this.keepaliveMs) || this.idleMs;
        this.waitMs = w;
        if (w > 0 && this.stream.wait) this.stream.wait(w);
    }

    // Bound the current phase: the stream's deadline, `ms` from now (0 clears).
    setDeadline(ms) {
        if (this.stream.deadline) this.stream.deadline(ms);
    }

    // A wait expired with nothing from the server. See the Lua twin.
    async quiet(mayPing) {
        this.quietMs = this.quietMs + this.waitMs;
        if (this.idleMs > 0 && this.quietMs >= this.idleMs) {
            this.dead = { code: "timeout",
                          detail: "nothing from the server for " + String(this.quietMs) + " ms" };
            fail(this.dead.code, this.dead.detail);
        }
        if (!(mayPing && this.authenticated && !this.inKex && this.keepaliveMs > 0)) {
            return;
        }
        if (this.keepaliveMax > 0 && this.unanswered >= this.keepaliveMax) {
            this.dead = { code: "timeout",
                          detail: String(this.unanswered) + " keepalives unanswered" };
            fail(this.dead.code, this.dead.detail);
        }
        this.unanswered = this.unanswered + 1;
        await this.sendPacket(KEEPALIVE);
    }

    // One stream call, with the transport marked as having it in flight for
    // its duration (see the Lua twin). The stream method is called BY NAME
    // from this module, so the native stream's caller gate sees stdlib.
    async streamCall(method, arg) {
        const s = this.stream;
        let call;
        if (method === "read") {
            call = () => s.read(arg);
        } else if (method === "write") {
            call = () => s.write(arg);
        } else {
            throw new Error("ssh: unknown stream call " + String(method));
        }
        // Another caller's call is in flight: refused HERE, with the flag
        // left as it is. Only the call that set the flag clears it.
        if (this.ioBusy) {
            fail("busy", "another coroutine is using this connection");
        }
        this.ioBusy = true;
        let r;
        try {
            r = await call();
        } catch (e) {
            this.ioBusy = false;
            throw e;
        }
        this.ioBusy = false;
        return r;
    }

    // One read from the stream, however long the server takes. Resolves to
    // 1..max bytes, or "" at EOF.
    async readSome(max) {
        for (let i = 0; i < 1000000; i++) {
            const [chunk, err, code] = await this.streamCall("read", max);
            if (chunk !== null && chunk !== undefined) {
                if (chunk !== "") { this.quietMs = 0; this.unanswered = 0; }
                return chunk;
            }
            if (code === "timeout") {
                await this.quiet(true);
            } else if (code === "deadline") {
                fail("deadline", "deadline reached");
            } else if (code === "busy") {
                fail("busy", String(err));
            } else {
                throw new Error("ssh: read failed: " + String(err));
            }
        }
        throw new Error("ssh: read made no progress");
    }

    // Pull at least `n` bytes into the buffer.
    // Each chunk goes into the buffer as it arrives: kept in a local until the
    // fill was done, it was lost when a later read of the same fill threw.
    async fill(n) {
        while (this.inbuf.length < n) {
            let want = n - this.inbuf.length;
            if (want > READ_CHUNK) want = READ_CHUNK;
            const chunk = await this.readSome(want);
            if (chunk === "") throw new Error("ssh: connection closed by peer");
            this.inbuf = this.inbuf + chunk;
        }
    }

    // Write all of `bytes`, retried under the liveness rules on a timeout.
    async sendRaw(bytes) {
        for (let i = 0; i < 1000000; i++) {
            const [ok, err, code] = await this.streamCall("write", bytes);
            if (ok) return;
            if (code === "timeout") {
                await this.quiet(false);
            } else if (code === "deadline") {
                fail("deadline", "deadline reached");
            } else if (code === "busy") {
                fail("busy", String(err));
            } else {
                throw new Error("ssh: write failed: " + String(err));
            }
        }
        throw new Error("ssh: write made no progress");
    }

    // One line of the identification exchange, without its CR LF.
    async readLine() {
        for (let i = 0; i < 64; i++) {
            const nl = this.inbuf.indexOf("\n");
            if (nl >= 0) {
                let line = this.inbuf.slice(0, nl);
                this.inbuf = this.inbuf.slice(nl + 1);
                if (line.endsWith("\r")) line = line.slice(0, -1);
                return line;
            }
            if (this.inbuf.length > packet.MAX_IDENT * 4) {
                throw new Error("ssh: no identification line within a sensible bound");
            }
            const chunk = await this.readSome(256);
            if (chunk === "") throw new Error("ssh: connection closed during identification");
            this.inbuf = this.inbuf + chunk;
        }
        throw new Error("ssh: too many lines before the identification string");
    }

    // Packets ---------------------------------------------------------------

    async sendPacket(payload) {
        if (this.dead) {
            // Coded like the failure that killed it.
            fail(this.dead.code || "dead",
                 "the connection is no longer usable"
                 + (this.dead.detail ? (": " + String(this.dead.detail)) : ""));
        }
        if (this.ioBusy || this.sendBusy) {
            fail("busy", "another coroutine is using this connection");
        }
        // The sequence number is spent HERE, before anything awaits: a packet
        // is sealed under it once, whether or not its write then succeeds (a
        // failed sealed write kills the connection below).
        const seq = this.sendSeq;
        this.sendSeq = (seq + 1) >>> 0;
        this.sendBusy = true;
        try {
            if (this.c2s) {
                const sealed = this.c2s.seal(this.aead, payload, this.crypto.random, seq);
                try {
                    await this.sendRaw(sealed);
                } catch (e) {
                    if (!this.dead) {
                        this.dead = { code: "send_failed",
                                      detail: "a sealed packet could not be written" };
                    }
                    throw e;
                }
            } else {
                await this.sendRaw(packet.frame(payload, 8, this.crypto.random));
            }
        } finally {
            this.sendBusy = false;
        }
    }

    // One packet off the wire: plaintext framing before NEWKEYS, the AEAD
    // after. The parser bounds the declared length before anything waits.
    // The read holds readBusy throughout, so no other reader consumes the
    // buffer or advances recvSeq / s2c under it (both are read only then).
    async readPacket() {
        if (this.readBusy) {
            fail("busy", "another coroutine is using this connection");
        }
        this.readBusy = true;
        try {
            await this.fill(4);
            const s2c = this.s2c, aead = this.aead, seq = this.recvSeq;
            const parse = (buf) => {
                if (s2c) return s2c.open(aead, buf, seq);
                return packet.parse(buf, 8);
            };
            let [payload, used] = parse(this.inbuf);
            if (payload === null || payload === undefined) {
                await this.fill(s2c ? s2c.needed(this.inbuf, seq)
                                    : wire.peekUint32(this.inbuf) + 4);
                [payload, used] = parse(this.inbuf);
            }
            this.inbuf = this.inbuf.slice(used);
            this.recvSeq = (seq + 1) >>> 0;
            return payload;
        } finally {
            this.readBusy = false;
        }
    }

    // Hold a message that arrived during a rekey WE started.
    deferMessage(p) {
        this.deferred.push(p);
    }

    takeDeferred() {
        const q = this.deferred;
        if (this.deferredHead >= q.length) return null;
        const p = q[this.deferredHead];
        q[this.deferredHead] = undefined;
        this.deferredHead = this.deferredHead + 1;
        if (this.deferredHead >= q.length) {
            this.deferred = [];
            this.deferredHead = 0;
        }
        return p;
    }

    // Anything set aside during a rekey comes out first, in order.
    async nextMessage(strict) {
        const held = this.takeDeferred();
        if (held !== null) return held;
        return await this.readMessage(strict);
    }

    // The next message off the WIRE, with transport chatter filtered.
    async readMessage(strict) {
        for (;;) {
            const p = await this.handlePacket(await this.readPacket(), strict);
            if (p !== null) return p;
        }
    }

    // A reply the transport owes the peer; held during a key exchange.
    async sendReply(payload) {
        if (this.inKex) {
            const q = this.heldReplies || [];
            if (q.length >= 64) {
                throw new Error("ssh: too many requests from the peer during key exchange");
            }
            q.push(payload);
            this.heldReplies = q;
            return;
        }
        await this.sendPacket(payload);
    }

    // One packet off the wire: returned when it is for the caller, null when
    // it was chatter the transport handled.
    async handlePacket(p, strict) {
        const m = p.charCodeAt(0);

        if (strict === "rekey") {
            if (m >= 50) {
                throw new Error("ssh: message " + String(m)
                    + " is not permitted during key exchange (strict KEX)");
            }
        } else if (strict && m !== SSH_MSG_DISCONNECT
                   && !(m === SSH_MSG_KEXINIT || m === kex.SSH_MSG_NEWKEYS
                        || (m >= 30 && m <= 49))) {
            throw new Error("ssh: message " + String(m)
                + " is not permitted during key exchange (strict KEX)");
        }

        if (m === SSH_MSG_KEXINIT && this.sessionId && !this.inKex) {
            // The server has asked to rekey: absorbed here.
            this.inKex = true;
            let ok, why;
            try {
                [ok, why] = await this.runKex(this.kexOpts || {}, p);
            } catch (e) {
                this.inKex = false;
                this.dead = { code: "kex_failed" };
                throw e;
            }
            this.inKex = false;
            if (!ok) {
                this.dead = { code: "kex_failed",
                              detail: (why && why.code) || "unknown" };
                throw new Error("ssh: rekey failed: "
                    + wire.safeName((why && why.code) || "unknown"));
            }
        } else if (m === SSH_MSG_DISCONNECT) {
            const r = wire.reader(p); r.byte();
            const code = r.uint32();
            const desc = r.remaining() > 0 ? r.string() : "";
            throw new Error("ssh: server disconnected (" + String(code) + "): "
                + wire.safeName(desc, 200));
        } else if (m === SSH_MSG_GLOBAL_REQUEST) {
            const r = wire.reader(p); r.byte(); r.string();
            if (r.boolean()) await this.sendReply(String.fromCharCode(SSH_MSG_REQUEST_FAILURE));
        } else if (m === channel.SSH_MSG_CHANNEL_OPEN) {
            // A server-initiated channel: refused, with a reply.
            const r = wire.reader(p); r.byte(); r.string();
            const sender = r.uint32();
            await this.sendReply(channel.buildOpenFailure(sender,
                channel.OPEN_ADMINISTRATIVELY_PROHIBITED,
                "hull/ssh accepts no server-initiated channels"));
        } else if (m !== SSH_MSG_IGNORE && m !== SSH_MSG_DEBUG
                   && m !== SSH_MSG_UNIMPLEMENTED
                   && m !== SSH_MSG_REQUEST_SUCCESS && m !== SSH_MSG_REQUEST_FAILURE) {
            return p;
        }
        return null;
    }

    // Whether the next message can be had without waiting on the peer.
    async messageReady() {
        if (this.deferredHead < this.deferred.length) return true;
        const whole = () => {
            const buf = this.inbuf;
            if (buf.length < 4) return false;
            return buf.length >= (this.s2c ? this.s2c.needed(buf, this.recvSeq)
                                           : wire.peekUint32(buf) + 4);
        };
        if (whole()) return true;
        if (!this.stream.wait) return false;
        if (this.readBusy) {
            fail("busy", "another coroutine is using this connection");
        }

        this.stream.wait(1);
        // The wait is put back however the read ends (audit 21).
        let r;
        try {
            r = await this.streamCall("read", READ_CHUNK);
        } catch (e) {
            this.stream.wait(this.waitMs > 0 ? this.waitMs : 0);
            throw e;
        }
        this.stream.wait(this.waitMs > 0 ? this.waitMs : 0);
        const [chunk, err, code] = r;
        if (chunk === null || chunk === undefined) {
            if (code === "timeout") return false;
            if (code === "deadline") fail("deadline", "deadline reached");
            throw new Error("ssh: read failed: " + String(err));
        }
        // End of stream: say ready, so the read that follows reports it.
        if (chunk === "") return true;
        this.quietMs = 0; this.unanswered = 0;
        this.inbuf = this.inbuf + chunk;
        return whole();
    }

    // The next message OFF THE WIRE must be exactly `want`. Key exchange only.
    async expect(want, what, strict) {
        const p = await this.readMessage(strict);
        const got = p.charCodeAt(0);
        if (got !== want) {
            throw new Error("ssh: expected " + what + " (" + String(want)
                + ") but the peer sent message " + String(got));
        }
        return p;
    }

    // -> [true] | [null, reason]. The host-key cases carry the fingerprint.
    async handshake(opts) {
        let vS = null;
        for (let i = 0; i < 32; i++) {
            vS = await this.readLine();
            if (vS.startsWith("SSH-")) break;
            if (opts.onBanner) await opts.onBanner(wire.safeText(vS));
            vS = null;
        }
        if (vS === null) return [null, { code: "no_identification" }];

        const [id] = packet.parseIdent(vS);
        if (!id) {
            return [null, { code: "bad_identification",
                            detail: wire.safeName(vS, 255) }];
        }
        await this.sendRaw(this.ident + "\r\n");
        this.serverIdent = vS;
        opts = await preferKnownKey(opts);
        this.kexOpts = opts;

        this.inKex = true;
        let ok, why;
        try {
            [ok, why] = await this.runKex(opts);
        } catch (e) {
            this.inKex = false;
            this.dead = { code: "kex_failed" };
            throw e;
        }
        this.inKex = false;
        if (!ok) {
            this.dead = why || { code: "kex_failed" };
            return [null, why];
        }
        return [true];
    }

    // One key exchange. `iS` is the server KEXINIT payload when the server
    // started the exchange; undefined when we are starting.
    // -> [true] | [null, reason]
    async runKex(opts, iS) {
        const rekey = this.sessionId !== null && this.sessionId !== undefined;

        const iC = kexinit.build(opts.offer, this.crypto.random(16));
        await this.sendPacket(iC);
        if (!iS) {
            if (rekey) {
                let held = 0;
                for (let i = 0; i < 4096; i++) {
                    const p = await this.readMessage();
                    const m = p.charCodeAt(0);
                    if (m === SSH_MSG_KEXINIT) { iS = p; break; }
                    if (m < 90 || m > 100) {
                        return [null, { code: "unexpected_message",
                                        detail: "message " + m + " before the peer's KEXINIT" }];
                    }
                    held = held + p.length;
                    if (held > DEFER_MAX_BYTES) {
                        return [null, { code: "no_kexinit_response",
                                        detail: "more than " + DEFER_MAX_BYTES
                                            + " bytes before the peer's KEXINIT" }];
                    }
                    this.deferMessage(p);
                }
                if (!iS) return [null, { code: "no_kexinit_response" }];
            } else {
                iS = await this.readMessage();
            }
        }

        const server = kexinit.parse(iS);
        let strict = this.strictKex;
        if (!rekey) strict = kexinit.serverIsStrict(server);
        const [neg, nerr] = kexinit.negotiate(opts.offer, server, strict);
        if (!neg) return [null, { code: "no_common_algorithm", detail: nerr }];
        this.negotiated = neg;
        if (!rekey) {
            this.strictKex = kexinit.serverIsStrict(server);
            if (this.strictKex && this.recvSeq !== 1) {
                throw new Error("ssh: strict KEX: the server's KEXINIT was not its first packet");
            }
        }
        let filter = this.strictKex;
        if (rekey && this.strictKex) filter = "rekey";

        if (kexinit.guessWasWrong(server, neg)) {
            await this.readMessage(filter);   // discard the guess (RFC 4253 7.1)
        }

        // curve25519 exchange
        const [qC, sk] = this.crypto.x25519Keypair();
        await this.sendPacket(kex.buildEcdhInit(qC));

        const reply = kex.parseEcdhReply(
            await this.expect(kex.SSH_MSG_KEX_ECDH_REPLY, "KEX_ECDH_REPLY", filter));

        const [kRaw, kerr] = this.crypto.x25519(sk, reply.qS);
        if (!kRaw) return [null, { code: "bad_kex_point", detail: kerr }];

        const h = this.rawSha(kex.exchangeHashInput({
            vC: this.ident, vS: this.serverIdent, iC: iC, iS: iS,
            kS: reply.hostKey, qC: qC, qS: reply.qS, k: kRaw,
        }));

        if (rekey) {
            // A rekey must re-present the SAME host key.
            if (reply.hostKey !== this.hostKeyBlob) {
                return [null, { code: "host_changed_midsession",
                                fingerprint: hostkey.fingerprint(this.rawSha, reply.hostKey),
                                storedFingerprint: this.hostFingerprint }];
            }
            const [ok, why] = hostkey.verifySignature(this.crypto, reply.hostKey,
                                                       reply.signature, h, neg.hostKey);
            if (!ok) return [null, { code: "host_key_invalid", detail: why }];
        } else {
            const d = await hostkey.verify(this.crypto,
                opts.trust, hostkey.storeName(opts.host, opts.port),
                reply.hostKey, reply.signature, h, neg.hostKey);
            if (!d.ok) return [null, { code: "host_key_invalid", detail: d.reason }];
            if (d.status !== hostkey.TRUSTED) {
                return [null, { code: "host_" + d.status,
                                fingerprint: d.fingerprint,
                                storedFingerprint: d.storedFingerprint,
                                keyBlob: d.keyBlob }];
            }
            this.hostFingerprint = d.fingerprint;
            this.hostKeyBlob = reply.hostKey;
            // RFC 4253 section 7.2: the session identifier never changes.
            this.sessionId = h;
        }

        const keys = kex.deriveKeys(this.rawSha, kRaw, h, this.sessionId,
                                    kex.sizesFor(neg.cipherC2s, neg.cipherS2c));

        if (this.c2s) this.totalSent = (this.totalSent || 0) + this.c2s.bytesProcessed();
        if (this.s2c) this.totalReceived = (this.totalReceived || 0) + this.s2c.bytesProcessed();

        await this.sendPacket(String.fromCharCode(kex.SSH_MSG_NEWKEYS));
        await this.expect(kex.SSH_MSG_NEWKEYS, "NEWKEYS", filter);
        if (this.strictKex) { this.sendSeq = 0; this.recvSeq = 0; }
        this.c2s = this.newCipher(neg.cipherC2s, keys.keyC2s, keys.ivC2s);
        this.s2c = this.newCipher(neg.cipherS2c, keys.keyS2c, keys.ivS2c);
        if (rekey) this.rekeys = this.rekeys + 1;
        const q = this.heldReplies;
        this.heldReplies = null;
        if (q) {
            for (const payload of q) await this.sendPacket(payload);
        }
        return [true];
    }

    // One direction's packet layer, for the cipher negotiated for it.
    newCipher(name, key, iv) {
        if (name === chacha.NAME) {
            const c = this.crypto;
            return chacha.new(key, {
                chacha20: (k, n, ctr, d) => c.chacha20(k, n, ctr, d),
                poly1305: (k, m) => c.poly1305(k, m),
                ctEq: (a, b) => c.constantTimeEq(a, b),
            });
        }
        return cipher.new(key, iv);
    }

    // Start a key exchange now. -> [true] | [null, reason]
    async rekey() {
        if (!this.sessionId) return [null, { code: "not_handshaken" }];
        if (this.dead) return [null, this.dead];
        if (this.inKex) return [true];          // one is already running
        if (this.ioBusy || this.sendBusy || this.readBusy) {
            return [null, { code: "busy",
                            detail: "another coroutine is using this connection" }];
        }
        this.inKex = true;
        let ok, why;
        try {
            [ok, why] = await this.runKex(this.kexOpts || {});
        } catch (e) {
            this.inKex = false;
            this.dead = { code: "kex_failed" };
            throw e;
        }
        this.inKex = false;
        if (!ok) {
            this.dead = why || { code: "kex_failed" };
            return [null, why];
        }
        return [true];
    }

    // Rekey if this connection has moved enough under the current keys.
    // Resolves to true if an exchange ran.
    async maybeRekey() {
        if (this.inKex || !this.sessionId) return false;
        if (!this.c2s || !this.s2c) return false;
        if (!(this.c2s.rekeyDue(this.rekeyLimit) || this.s2c.rekeyDue(this.rekeyLimit))) {
            return false;
        }
        const [ok, why] = await this.rekey();
        if (!ok && why && why.code === "busy") return false;
        if (!ok) {
            throw new Error("ssh: rekey failed: "
                + wire.safeName((why && why.code) || "unknown"));
        }
        return true;
    }

    // What this connection has moved, and how many times it has re-keyed.
    stats() {
        const sent = (this.totalSent || 0) + (this.c2s ? this.c2s.bytesProcessed() : 0);
        const recv = (this.totalReceived || 0) + (this.s2c ? this.s2c.bytesProcessed() : 0);
        return {
            rekeys: this.rekeys,
            authKey: this.authKey,
            bytesSent: sent,
            bytesReceived: recv,
            packetsSent: this.c2s ? this.c2s.packetsSent() : 0,
            packetsReceived: this.s2c ? this.s2c.packetsSent() : 0,
            rekeyDue: (this.c2s !== null && this.c2s !== undefined && this.c2s.rekeyDue(this.rekeyLimit))
                || (this.s2c !== null && this.s2c !== undefined && this.s2c.rekeyDue(this.rekeyLimit)),
        };
    }

    // `keys` is one loaded key, or a list of them offered in order.
    // -> [true] | [null, reason]
    async authenticate(user, keys, onBanner) {
        await this.sendPacket(userauth.buildServiceRequest());
        const accepted = userauth.parseServiceAccept(await this.nextMessage());
        if (accepted !== userauth.SERVICE_USERAUTH) {
            return [null, { code: "service_refused", detail: wire.safeName(accepted) }];
        }

        if (!Array.isArray(keys)) keys = [keys];
        let err;
        for (let n = 0; n < keys.length; n++) {
            const [ok, e, nextKey] = await offerKey(this, user, keys[n], onBanner);
            if (ok) {
                this.user = user;
                this.authKey = n;                  // 0-based (Lua: 1-based); see Conn.stats()
                this.authenticated = true;         // keepalives may now be sent
                return [true];
            }
            err = e;
            if (!nextKey) break;
        }
        if (err && err.code === "auth_failed" && keys.length > 1) {
            err.detail = "none of the " + String(keys.length) + " keys was accepted";
        }
        return [null, err];
    }

    // Open an SFTP session on its own channel. See hull:ssh:sftp_client.
    async sftp(opts) {
        return await sftpClient.open(this, opts);
    }

    close() {
        // Swallowed deliberately: the last act on a finished connection.
        try { this.stream.close(); } catch (e) { /* see the Lua twin */ }
    }
}

// `opts` with the host key algorithms reordered so that the type the trust
// store already holds for this host comes first. A store that cannot be read
// here is left alone.
async function preferKnownKey(opts) {
    const store = opts.trust;
    if (!store || typeof store !== "object" || typeof store.get !== "function") return opts;
    let blob;
    try {
        blob = await store.get(hostkey.storeName(opts.host, opts.port));
    } catch (e) {
        return opts;
    }
    if (typeof blob !== "string") return opts;
    const wanted = {};
    for (const a of hostkey.algorithmsFor(hostkey.keyType(blob))) wanted[a] = true;

    const base = opts.offer || kexinit.DEFAULT_OFFER;
    const first = [], rest = [];
    for (const a of base.hostKey) {
        if (wanted[a]) first.push(a); else rest.push(a);
    }
    if (first.length === 0 || (first[0] === base.hostKey[0] && first.length === 1)) return opts;
    for (const a of rest) first.push(a);

    const offer = Object.assign({}, base);
    offer.hostKey = first;
    const out = Object.assign({}, opts);
    out.offer = offer;
    return out;
}

// Offer one key, under each signature algorithm it has.
// -> [true] | [null, failure, nextKey]
async function offerKey(self, user, key, onBanner) {
    const algorithms = key.algorithm === "ssh-rsa" ? ["rsa-sha2-512", "rsa-sha2-256"]
        : [key.algorithm || userauth.ALGORITHM];

    for (let i = 0; i < algorithms.length; i++) {
        const alg = algorithms[i];
        const blob = userauth.signedBlob(self.sessionId, user, key.blob, alg);
        let sig;
        const a = hostkey.ALGORITHMS[alg];
        if (a && a.jose) {
            sig = self.crypto.sign(a.jose, key.pem, blob);
        } else {
            sig = self.crypto.ed25519Sign(blob, key.secret);
        }
        await self.sendPacket(userauth.buildRequest(user, key.blob,
                                                    userauth.signatureBlob(sig, alg), alg));

        let failure = null;
        for (let k = 0; k < 16; k++) {
            const r = userauth.parseResponse(await self.nextMessage());
            if (r.type === "banner") {
                if (onBanner) await onBanner(r.message);
            } else if (r.type === "success") {
                return [true];
            } else if (r.type === "failure") {
                failure = r;
                break;
            }
        }
        if (!failure) return [null, { code: "no_auth_response" }, false];
        if (failure.partial) {
            return [null, { code: "partial_success", methods: failure.methods }, false];
        }
        if (!userauth.canRetryPublickey(failure)) {
            return [null, { code: "auth_failed", methods: failure.methods }, false];
        }
        if (i === algorithms.length - 1) {
            return [null, { code: "auth_failed", methods: failure.methods }, true];
        }
    }
    return [null];
}

function newTransport(stream, crypto, opts) {
    opts = opts || {};
    const opt = (v, d) => (v === undefined || v === null) ? d : v;
    const t = new Transport();
    t.keepaliveMs = opt(opts.keepaliveMs, KEEPALIVE_MS);
    t.keepaliveMax = opt(opts.keepaliveMax, KEEPALIVE_MAX);
    t.idleMs = opt(opts.idleMs, IDLE_MS);
    t.quietMs = 0;
    t.unanswered = 0;
    t.stream = stream;
    t.crypto = crypto;
    // build_ident validates the software string; stored without the CR LF.
    t.ident = packet.buildIdent(opts.software || "Hull").slice(0, -2);
    t.inbuf = "";
    t.nextChannel = 0;
    t.channels = {};
    t.rekeys = 0;
    t.recvSeq = 0;
    t.sendSeq = 0;
    t.deferred = [];
    t.deferredHead = 0;
    t.rekeyLimit = opts.rekeyLimit;
    t.aead = {
        seal: (k, iv, aad, p) => crypto.gcmSeal(k, iv, aad, p),
        open: (k, iv, aad, c, tg) => crypto.gcmOpen(k, iv, aad, c, tg),
    };
    t.rawSha = crypto.sha256;
    t.ioBusy = false;
    t.sendBusy = false;
    t.readBusy = false;
    t.applyWait();
    return t;
}

// Channels: sessions, exec and the routing between channels live in
// hull:ssh:session, installed onto this class.
session.install(Transport);

export const transport = {
    new: newTransport,
    KEEPALIVE_MS,
    KEEPALIVE_MAX,
    IDLE_MS,
};
export default transport;
