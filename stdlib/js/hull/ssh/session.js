// hull:ssh:session - channels on an established connection: open a session,
// run a command on it, route what arrives to the channel it belongs to, and
// close channels so the next operation starts clean.
//
// The JS twin of hull.ssh.session (stdlib/lua/hull/ssh/session.lua); see
// there for the reasoning behind each rule. These are methods of the
// Transport class, installed onto its prototype by `install(Transport)`, so
// callers write `t.exec(...)` and the sftp client calls `t.openSession()`.
// Everything here works in messages (`sendPacket`, `nextMessage`).

import { channel } from "hull:ssh:channel";

// How much stdin exec will write to a command (see the Lua twin).
const STDIN_MAX = 128 * 1024;

// How long closing a timed-out command's channel may take.
const CHANNEL_DRAIN_MS = 5000;

// Replies to channel requests that may wait for one idle channel.
const MAX_QUEUED_REPLIES = 64;

function sshError(code, detail, extra) {
    const e = new Error(detail === undefined || detail === null ? code : String(detail));
    e.code = code;
    if (detail !== undefined) e.detail = detail;
    if (extra) for (const k of Object.keys(extra)) e[k] = extra[k];
    return e;
}

// Whether `m` is charged against the receive window.
function isWindowed(m) {
    return m.type === "data" || m.type === "extended_data";
}

// What the owner of an idle channel needs to see of a message that arrived
// while another channel was being read. -> [kind, key]
const ONCE_REQUESTS = { "exit-status": true, "exit-signal": true };
function queueKind(m) {
    const ty = m.type;
    if (isWindowed(m)) return ["data"];
    if (ty === "eof" || ty === "close" || ty === "open_confirmation"
        || ty === "open_failure") {
        return ["once", ty];
    }
    if (ty === "request" && ONCE_REQUESTS[m.request] === true) {
        return ["once", m.request];
    }
    if (ty === "request_success" || ty === "request_failure") return ["reply"];
    return [null];
}

const Session = {
    // -> [ch] | [null, reason]. The one gateway every operation passes
    // through, and the one moment nothing is in flight.
    async openSession() {
        if (this.dead) return [null, this.dead];
        await this.maybeRekey();

        const ch = channel.new({ id: this.nextChannel });
        this.nextChannel = this.nextChannel + 1;
        // Registered before the open goes out (see readFor).
        this.channels[ch.localId] = ch;
        await this.sendPacket(ch.openMessage());

        for (let i = 0; i < 16; i++) {
            const m = await this.readFor(ch);
            if (m.type === "open_confirmation") {
                ch.handle(m); return [ch];
            } else if (m.type === "open_failure") {
                ch.handle(m);
                delete this.channels[ch.localId];
                return [null, { code: "channel_refused", detail: m.description }];
            }
        }
        delete this.channels[ch.localId];
        return [null, { code: "no_channel_response" }];
    },

    // Read and discard what the peer still has to say about a channel we
    // have given up on, up to and including its CHANNEL_CLOSE. Bounded, and
    // tolerant of a read that fails.
    async drainChannel(ch) {
        if (ch.closed) return;
        for (let i = 0; i < 10000; i++) {
            let m;
            try {
                m = await this.readFor(ch);
            } catch (e) {
                return;
            }
            if (m.recipient === ch.localId) {
                if (!m.routed) { try { ch.handle(m); } catch (e) { /* tolerated */ } }
                if (m.type === "close") return;
            }
        }
        this.close();
    },

    // Close channel `ch` from our side and read it through to the peer's
    // CLOSE. Once only.
    async closeChannel(ch) {
        if (ch.closeSent) return;
        ch.closeSent = true;
        try {
            await this.sendPacket(channel.buildClose(ch.remoteId));
            await this.drainChannel(ch);
        } catch (e) { /* see the Lua twin: tolerated */ }
        delete this.channels[ch.localId];
    },

    // The next connection message, applied to channel `ch`. A request
    // wanting a reply gets CHANNEL_FAILURE.
    async channelMessage(ch) {
        const m = await this.readFor(ch);
        if (!m.routed) {
            ch.handle(m);
            if (m.type === "request" && m.wantReply
                && ch.remoteId !== null && ch.remoteId !== undefined) {
                await this.sendPacket(channel.buildFailure(ch.remoteId));
            }
        }
        return m;
    },

    // The next channel message for `ch`, with the others on the connection
    // kept where they belong. A queued message comes back with `routed` set.
    async readFor(ch) {
        const q = ch.inbox;
        if (ch.inboxHead < q.length) {
            const m = q[ch.inboxHead];
            q[ch.inboxHead] = undefined;
            ch.inboxHead = ch.inboxHead + 1;
            if (queueKind(m)[0] === "reply") {
                ch.inboxCtl = (ch.inboxCtl === undefined || ch.inboxCtl === null
                               ? 1 : ch.inboxCtl) - 1;
            }
            if (ch.inboxHead >= q.length) { ch.inbox = []; ch.inboxHead = 0; }
            return m;
        }
        for (;;) {
            const m = await this.route(ch, channel.parse(await this.nextMessage()));
            if (m) return m;
            // route() may have found the connection finished.
            if (this.dead) throw sshError(this.dead.code, this.dead.detail);
        }
    },

    // `m` if it is for `ch`; otherwise applied to (and queued for) the open
    // channel it belongs to, and null.
    async route(ch, m) {
        const other = m.recipient !== ch.localId && this.channels[m.recipient];
        if (!other) return m;
        other.handle(m);
        if (m.type === "request" && m.wantReply
            && other.remoteId !== null && other.remoteId !== undefined) {
            await this.sendPacket(channel.buildFailure(other.remoteId));
        }
        const [kind, key] = queueKind(m);
        if (kind === "once") {
            other.queuedOnce = other.queuedOnce || {};
            if (other.queuedOnce[key]) return null;
            other.queuedOnce[key] = true;
        } else if (kind === "reply") {
            const n = (other.inboxCtl || 0) + 1;
            if (n > MAX_QUEUED_REPLIES) {
                // The peer is not following the protocol: the CONNECTION is
                // finished, not this one unrelated read.
                if (!this.dead) {
                    this.dead = { code: "protocol_error",
                                  detail: "too many replies queued for channel "
                                      + String(other.localId) };
                }
                try { this.close(); } catch (e) { /* tolerated */ }
                return null;
            }
            other.inboxCtl = n;
        } else if (kind === null) {
            return null;
        }
        m.routed = true;
        other.inbox.push(m);
        return null;
    },

    // The next channel message for `ch` IF one can be had without waiting
    // on the peer, else null.
    async pollChannel(ch) {
        while (await this.messageReady()) {
            let p = this.takeDeferred();
            if (p === null) p = await this.handlePacket(await this.readPacket());
            if (p !== null) {
                const m = await this.route(ch, channel.parse(p));
                if (m) {
                    ch.handle(m);
                    if (m.type === "request" && m.wantReply
                        && ch.remoteId !== null && ch.remoteId !== undefined) {
                        await this.sendPacket(channel.buildFailure(ch.remoteId));
                    }
                    return m;
                }
                if (this.dead) throw sshError(this.dead.code, this.dead.detail);
            }
        }
        return null;
    },

    // Wait for the reply to a channel request.
    async awaitChannelReply(ch) {
        for (let i = 0; i < 32; i++) {
            const m = await this.channelMessage(ch);
            if (m.type === "request_success") return true;
            if (m.type === "request_failure") return false;
        }
        throw new Error("ssh: no reply to the channel request");
    },

    // Run a command. -> [{ status, signal, stdout, stderr }] | [null, reason]
    // See the Lua twin for the stdout/stderr delivery and stdin rules.
    async exec(command, opts) {
        opts = opts || {};
        const onStdout = opts.onStdout, onStderr = opts.onStderr;

        const stdin = opts.stdin;
        if (stdin !== undefined && stdin !== null && typeof stdin !== "string"
            && typeof stdin !== "function") {
            return [null, { code: "bad_stdin", detail: typeof stdin }];
        }
        const timeout = opts.timeoutMs;
        if (timeout !== undefined && timeout !== null
            && (!Number.isInteger(timeout) || timeout < 1 || timeout > 86400000)) {
            return [null, { code: "bad_timeout", detail: String(timeout) }];
        }

        const [ch, cerr] = await this.openSession();
        if (!ch) return [null, cerr];

        const abort = async (reason) => {
            await this.closeChannel(ch);
            return [null, reason];
        };

        await this.sendPacket(channel.buildExec(ch.remoteId, command));
        if (!(await this.awaitChannelReply(ch))) {
            return await abort({ code: "exec_refused", detail: command });
        }

        const out = [], errout = [];
        const limit = opts.maxOutput || (8 * 1024 * 1024);
        let buffered = 0;
        let overflow = false;

        const deliver = async (m) => {
            let cb, into;
            if (m.type === "data") {
                cb = onStdout; into = out;
            } else if (m.type === "extended_data" && m.stderr) {
                cb = onStderr; into = errout;
            } else {
                return;
            }
            if (cb) {
                // The caller's own error stays the caller's.
                try {
                    await cb(m.data);
                } catch (cbErr) {
                    throw { appError: cbErr };
                }
                return;
            }
            buffered = buffered + m.data.length;
            if (buffered > limit) { overflow = true; return; }
            into.push(m.data);
        };

        let peerClosed = false;

        const pump = async () => {
            const m = await this.channelMessage(ch);
            if (m.type === "close") peerClosed = true;
            await deliver(m);
            const adj = ch.windowAdjustment();
            if (adj) await this.sendPacket(adj);
            return m;
        };

        // Take in whatever the command has already sent, without waiting.
        const drain = async () => {
            while (!peerClosed) {
                const m = await this.pollChannel(ch);
                if (!m) return null;
                if (m.type === "close") peerClosed = true;
                await deliver(m);
                const adj = ch.windowAdjustment();
                if (adj) await this.sendPacket(adj);
                if (overflow) return { code: "output_too_large", limit: limit };
            }
            return null;
        };

        // Write `data` within the peer's window. -> failure | null
        const sendAll = async (data) => {
            let sent = 0;
            while (sent < data.length && !ch.closed) {
                const f = await drain();
                if (f) return f;
                let room = ch.sendable();
                while (room <= 0 && !ch.closed) {
                    await pump();
                    if (overflow) return { code: "output_too_large", limit: limit };
                    room = ch.sendable();
                }
                if (ch.closed) break;
                const chunk = data.slice(sent, sent + room);
                await this.sendPacket(ch.dataMessage(chunk));
                sent = sent + chunk.length;
            }
            return null;
        };

        const drive = async () => {
            if (typeof stdin === "string") {
                if (stdin.length > STDIN_MAX) {
                    return { code: "stdin_too_large", limit: STDIN_MAX,
                             detail: "use sftp for bulk data, or pass a function to stream it" };
                }
                const f = await sendAll(stdin);
                if (f) return f;
            } else if (stdin) {
                while (!ch.closed) {
                    let chunk;
                    try {
                        chunk = await stdin();
                    } catch (sErr) {
                        throw { appError: sErr };
                    }
                    if (chunk === undefined || chunk === null || chunk === "") break;
                    if (typeof chunk !== "string") {
                        return { code: "bad_stdin",
                                 detail: "the stdin source returned a " + typeof chunk };
                    }
                    const f = await sendAll(chunk);
                    if (f) return f;
                }
            }
            if (stdin && !ch.closed) {
                await this.sendPacket(channel.buildEof(ch.remoteId));
            }
            while (!peerClosed) {
                await pump();
                if (overflow) return { code: "output_too_large", limit: limit };
            }
            return null;
        };

        if (timeout) this.setDeadline(timeout);
        let failure;
        try {
            failure = await drive();
        } catch (e) {
            if (timeout) this.setDeadline(0);
            if (e && typeof e === "object" && e.code === "deadline") {
                // The COMMAND ran out of time; the connection may be fine.
                this.setDeadline(CHANNEL_DRAIN_MS);
                await this.closeChannel(ch);
                this.setDeadline(0);
                if (!ch.closed) {
                    this.dead = { code: "timeout",
                                  detail: "a timed-out command's channel did not close" };
                    this.close();
                }
                return [null, { code: "timeout",
                                detail: "the command did not finish within "
                                    + String(timeout) + " ms" }];
            }
            await this.closeChannel(ch);
            throw e;
        }
        if (timeout) this.setDeadline(0);
        if (failure) return await abort(failure);
        await this.closeChannel(ch);

        const res = ch.result();
        return [{ status: res.status, signal: res.signal,
                  stdout: out.join(""), stderr: errout.join("") }];
    },
};

// Copy the session methods onto the Transport class.
function install(Transport) {
    for (const name of Object.keys(Session)) {
        Transport.prototype[name] = Session[name];
    }
}

export const session = { install };
export default session;
