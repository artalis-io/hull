// hull:ssh:channel - the connection protocol: channels, exec, flow control.
//
// The JS twin of hull.ssh.channel (stdlib/lua/hull/ssh/channel.lua); see
// there for the reasoning. RFC 4254. Flow control is the bound on how much a
// peer can make Hull hold: data beyond the advertised window is a hard error.
//
// No I/O: this builds and interprets messages and tracks the counters.

import { wire } from "hull:ssh:wire";

const SSH_MSG_CHANNEL_OPEN              = 90;
const SSH_MSG_CHANNEL_OPEN_CONFIRMATION = 91;
const SSH_MSG_CHANNEL_OPEN_FAILURE      = 92;
const SSH_MSG_CHANNEL_WINDOW_ADJUST     = 93;
const SSH_MSG_CHANNEL_DATA              = 94;
const SSH_MSG_CHANNEL_EXTENDED_DATA     = 95;
const SSH_MSG_CHANNEL_EOF               = 96;
const SSH_MSG_CHANNEL_CLOSE             = 97;
const SSH_MSG_CHANNEL_REQUEST           = 98;
const SSH_MSG_CHANNEL_SUCCESS           = 99;
const SSH_MSG_CHANNEL_FAILURE           = 100;

// RFC 4254 section 5.2: the only extended data type defined is stderr.
const EXTENDED_DATA_STDERR = 1;

const DEFAULT_WINDOW     = 2 * 1024 * 1024;
const DEFAULT_MAX_PACKET = 32 * 1024;

const isSet = (v) => v !== undefined && v !== null;

// Builders -------------------------------------------------------------------

function buildOpen(senderId, window, maxPacket) {
    return wire.writer()
        .byte(SSH_MSG_CHANNEL_OPEN)
        .string("session")
        .uint32(senderId)
        .uint32(isSet(window) ? window : DEFAULT_WINDOW)
        .uint32(isSet(maxPacket) ? maxPacket : DEFAULT_MAX_PACKET)
        .build();
}

function buildExec(recipientId, command, wantReply) {
    if (typeof command !== "string") {
        throw new TypeError("ssh.channel: exec needs a command string");
    }
    // The command is ONE length-prefixed blob: no quoting anywhere.
    return wire.writer()
        .byte(SSH_MSG_CHANNEL_REQUEST)
        .uint32(recipientId)
        .string("exec")
        .boolean(wantReply !== false)
        .string(command)
        .build();
}

function buildSubsystem(recipientId, name, wantReply) {
    return wire.writer()
        .byte(SSH_MSG_CHANNEL_REQUEST)
        .uint32(recipientId)
        .string("subsystem")
        .boolean(wantReply !== false)
        .string(name)
        .build();
}

function buildData(recipientId, data) {
    return wire.writer()
        .byte(SSH_MSG_CHANNEL_DATA)
        .uint32(recipientId)
        .string(data)
        .build();
}

function buildEof(recipientId) {
    return wire.writer().byte(SSH_MSG_CHANNEL_EOF).uint32(recipientId).build();
}

function buildClose(recipientId) {
    return wire.writer().byte(SSH_MSG_CHANNEL_CLOSE).uint32(recipientId).build();
}

// The answer to a channel request we do not act on (RFC 4254 section 5.4).
function buildFailure(recipientId) {
    return wire.writer().byte(SSH_MSG_CHANNEL_FAILURE).uint32(recipientId).build();
}

// Refuse a channel the SERVER asked to open (RFC 4254 section 5.1).
const OPEN_ADMINISTRATIVELY_PROHIBITED = 1;

function buildOpenFailure(recipientId, reason, description) {
    return wire.writer()
        .byte(SSH_MSG_CHANNEL_OPEN_FAILURE)
        .uint32(recipientId)
        .uint32(reason)
        .string(description || "")
        .string("")
        .build();
}

function buildWindowAdjust(recipientId, add) {
    return wire.writer()
        .byte(SSH_MSG_CHANNEL_WINDOW_ADJUST)
        .uint32(recipientId)
        .uint32(add)
        .build();
}

// Parsing --------------------------------------------------------------------

// Parse one connection-protocol message into an object with a `type`.
// Throws on anything malformed.
function parse(payload) {
    const r = wire.reader(payload);
    const msg = r.byte();

    if (msg === SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
        const recipient = r.uint32(), sender = r.uint32();
        const window = r.uint32(), maxPacket = r.uint32();
        return { type: "open_confirmation", recipient, sender, window, maxPacket };
    }

    if (msg === SSH_MSG_CHANNEL_OPEN_FAILURE) {
        const recipient = r.uint32();
        const reason = r.uint32();
        // Server text that reaches a caller error report.
        const description = wire.safeName(r.string(), 200);
        const language = r.string();
        return { type: "open_failure", recipient, reason, description, language };
    }

    if (msg === SSH_MSG_CHANNEL_WINDOW_ADJUST) {
        const recipient = r.uint32();
        return { type: "window_adjust", recipient, add: r.uint32() };
    }

    if (msg === SSH_MSG_CHANNEL_DATA) {
        const recipient = r.uint32();
        return { type: "data", recipient, data: r.string() };
    }

    if (msg === SSH_MSG_CHANNEL_EXTENDED_DATA) {
        const recipient = r.uint32();
        const code = r.uint32();
        return { type: "extended_data", recipient, code, data: r.string(),
                 stderr: code === EXTENDED_DATA_STDERR };
    }

    if (msg === SSH_MSG_CHANNEL_EOF) return { type: "eof", recipient: r.uint32() };
    if (msg === SSH_MSG_CHANNEL_CLOSE) return { type: "close", recipient: r.uint32() };
    if (msg === SSH_MSG_CHANNEL_SUCCESS) return { type: "request_success", recipient: r.uint32() };
    if (msg === SSH_MSG_CHANNEL_FAILURE) return { type: "request_failure", recipient: r.uint32() };

    if (msg === SSH_MSG_CHANNEL_REQUEST) {
        const recipient = r.uint32();
        const what = r.string();
        const wantReply = r.boolean();
        const out = { type: "request", recipient, request: what, wantReply };
        if (what === "exit-status") {
            out.exitStatus = r.uint32();
        } else if (what === "exit-signal") {
            // Peer text, cleaned of control characters here, once.
            out.signal = wire.safeName(r.string());
            out.coreDumped = r.boolean();
            out.errorMessage = wire.safeText(r.string());
            out.language = wire.safeName(r.string());
        }
        // Any other request type is returned unparsed rather than refused.
        return out;
    }

    throw new Error("ssh.channel: unexpected connection message " + String(msg));
}

// Channel state --------------------------------------------------------------

// The most data we put in one CHANNEL_DATA, whatever the peer allows.
const MAX_SEND = 32768;

class Channel {
    constructor(opts) {
        opts = opts || {};
        this.localId     = isSet(opts.id) ? opts.id : 0;
        this.remoteId    = null;
        this.recvWindow  = isSet(opts.window) ? opts.window : DEFAULT_WINDOW;
        this.recvInitial = this.recvWindow;
        this.sendWindow  = 0;               // until the peer tells us
        this.maxPacket   = isSet(opts.maxPacket) ? opts.maxPacket : DEFAULT_MAX_PACKET;
        this.sendMaxPacket = null;
        this.open        = false;
        this.eofReceived = false;
        this.closed      = false;
        this.exitStatus  = null;
        this.exitSignal  = null;
        // Messages that arrived while another channel was being read.
        this.inbox = [];
        this.inboxHead = 0;                 // 0-based (Lua: 1-based)
        this.inboxCtl = 0;                  // queued request replies (hull:ssh:session)
        this.queuedOnce = {};               // one-shot events already queued
    }

    openMessage() {
        return buildOpen(this.localId, this.recvWindow, this.maxPacket);
    }

    // Every inbound message for this channel goes through here. Returns the
    // parsed event; throws on a protocol violation.
    handle(msg) {
        if (isSet(msg.recipient) && msg.recipient !== this.localId) {
            throw new Error("ssh.channel: message for channel " + String(msg.recipient)
                + ", expected " + String(this.localId));
        }

        if (msg.type === "open_confirmation") {
            this.remoteId      = msg.sender;
            this.sendWindow    = msg.window;
            this.sendMaxPacket = msg.maxPacket;
            this.open          = true;
            return msg;
        }

        if (msg.type === "open_failure") {
            this.closed = true;
            return msg;
        }

        if (msg.type === "window_adjust") {
            const grown = this.sendWindow + msg.add;
            if (grown > 0xFFFFFFFF) {
                throw new Error("ssh.channel: window adjust overflows the window");
            }
            this.sendWindow = grown;
            return msg;
        }

        if (msg.type === "data" || msg.type === "extended_data") {
            const n = msg.data.length;
            if (n > this.maxPacket) {
                throw new Error("ssh.channel: peer sent " + String(n)
                    + " bytes, over the " + String(this.maxPacket) + " byte packet cap");
            }
            if (n > this.recvWindow) {
                throw new Error("ssh.channel: peer sent " + String(n)
                    + " bytes, over the remaining window of " + String(this.recvWindow));
            }
            this.recvWindow -= n;
            return msg;
        }

        if (msg.type === "eof") {
            this.eofReceived = true;
            return msg;
        }

        if (msg.type === "close") {
            this.closed = true;
            return msg;
        }

        if (msg.type === "request") {
            if (msg.request === "exit-status") {
                this.exitStatus = msg.exitStatus;
            } else if (msg.request === "exit-signal") {
                // A command killed by a signal has no exit status.
                this.exitSignal = {
                    signal: msg.signal,
                    coreDumped: msg.coreDumped,
                    message: msg.errorMessage,
                };
            }
            return msg;
        }

        return msg;
    }

    // Whether n bytes fit in what the peer has granted, and under its cap.
    canSend(n) {
        if (!this.open || this.closed) return false;
        if (isSet(this.sendMaxPacket) && n > this.sendMaxPacket) return false;
        return n <= this.sendWindow;
    }

    // The largest chunk that can be sent right now (0 = wait for an adjust).
    sendable() {
        if (!this.open || this.closed) return 0;
        let n = this.sendWindow;
        if (isSet(this.sendMaxPacket) && this.sendMaxPacket < n) n = this.sendMaxPacket;
        if (n > MAX_SEND) n = MAX_SEND;
        return n;
    }

    // Build a data message and account for it; refuses rather than truncates.
    dataMessage(data) {
        if (!this.canSend(data.length)) {
            throw new Error("ssh.channel: " + String(data.length)
                + " bytes does not fit the current window/packet limit");
        }
        this.sendWindow -= data.length;
        return buildData(this.remoteId, data);
    }

    // Top up the receive window once it has dropped to half. `unconsumed` is
    // what the caller holds but has not taken in: it still counts. Returns a
    // message, or null when no adjustment is due.
    windowAdjustment(unconsumed) {
        if (!this.open || this.closed) return null;
        if (this.recvWindow > Math.floor(this.recvInitial / 2)) return null;
        const add = this.recvInitial - this.recvWindow - (unconsumed || 0);
        if (add <= 0) return null;
        this.recvWindow += add;
        return buildWindowAdjust(this.remoteId, add);
    }

    // The result of a finished command. `status` is null when killed by a
    // signal; look at `signal` then.
    result() {
        return { status: this.exitStatus, signal: this.exitSignal,
                 eof: this.eofReceived, closed: this.closed };
    }
}

function newChannel(opts) {
    return new Channel(opts);
}

export const channel = {
    SSH_MSG_CHANNEL_OPEN, SSH_MSG_CHANNEL_OPEN_CONFIRMATION, SSH_MSG_CHANNEL_OPEN_FAILURE,
    SSH_MSG_CHANNEL_WINDOW_ADJUST, SSH_MSG_CHANNEL_DATA, SSH_MSG_CHANNEL_EXTENDED_DATA,
    SSH_MSG_CHANNEL_EOF, SSH_MSG_CHANNEL_CLOSE, SSH_MSG_CHANNEL_REQUEST,
    SSH_MSG_CHANNEL_SUCCESS, SSH_MSG_CHANNEL_FAILURE,
    EXTENDED_DATA_STDERR, DEFAULT_WINDOW, DEFAULT_MAX_PACKET,
    OPEN_ADMINISTRATIVELY_PROHIBITED, MAX_SEND,
    buildOpen, buildExec, buildSubsystem, buildData, buildEof, buildClose,
    buildFailure, buildOpenFailure, buildWindowAdjust, parse,
    new: newChannel,
};
export default channel;
