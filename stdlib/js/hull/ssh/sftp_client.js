// hull:ssh:sftp_client - an SFTP session over one channel of a connection.
//
// The JS twin of hull.ssh.sftp_client (stdlib/lua/hull/ssh/sftp_client.lua);
// see there for the reasoning behind each rule. The codec is hull:ssh:sftp
// (bytes <-> objects, no I/O); this is the client on top: a session on its own
// channel, requests matched to their replies by id, and the operations an
// application calls. It moves bytes only through the connection it is given
// (hull:ssh:transport), so it holds no authority of its own.
//
// Internal: applications reach it through a connection's sftp() method, which
// hands out a handle rather than this object.
//
// Shapes (PORTING): a method that can fail with a reason returns
// [value] | [null, reason]; a caller's mistake throws a coded Error
// ("bad_argument"); a broken session throws.

import { channel } from "hull:ssh:channel";
import { wire } from "hull:ssh:wire";
import { sftp } from "hull:ssh:sftp";

function sshError(code, detail, extra) {
    const e = new Error(detail === undefined || detail === null ? code : String(detail));
    e.code = code;
    if (detail !== undefined) e.detail = detail;
    if (extra) for (const k of Object.keys(extra)) e[k] = extra[k];
    return e;
}

// A caller's mistake (a bad mode, a negative offset), thrown with a code so
// the facade hands it back as { code: "bad_argument" }.
function badArgument(detail) {
    throw sshError("bad_argument", detail);
}

// A reply that is not the one asked for, as the error a caller branches on.
function sftpError(r) {
    if (r.type === "status") {
        return { code: r.name, detail: r.text, status: r.code };
    }
    return { code: "bad_reply", detail: "unexpected " + String(r.type) };
}

const REPLY_TIMEOUT_MS = 60000;
const ABANDON_MS = 5000;

const isInt = (v) => typeof v === "number" && Number.isInteger(v);

class Sftp {
    constructor(t, ch, replyMs) {
        this.t = t;
        this.ch = ch;
        this.buf = "";
        this.chunks = [];
        this.chunked = 0;
        this.id = 0;
        this.replyMs = replyMs;
        this.broken = null;
        this.version = undefined;
    }

    // Throw a broken session's reason (a string or a coded object).
    _throwBroken() {
        const b = this.broken;
        if (typeof b === "string") throw new Error(b);
        throw b;
    }

    // Send one SFTP message, split across as many CHANNEL_DATA messages as the
    // peer's window and packet size require.
    async send(payload) {
        if (this.broken) this._throwBroken();
        const bytes = sftp.frame(payload);
        let off = 0;
        while (off < bytes.length) {
            let room = this.ch.sendable();
            while (room <= 0) {
                await this.pump();
                room = this.ch.sendable();
            }
            const chunk = bytes.slice(off, off + room);
            await this.t.sendPacket(this.ch.dataMessage(chunk));
            off += chunk.length;
        }
    }

    // Bytes received for this session and not yet consumed.
    held() {
        return this.buf.length + this.chunked;
    }

    // Top up the window we grant, by what has been CONSUMED.
    async grant() {
        const adj = this.ch.windowAdjustment(this.held());
        if (adj) await this.t.sendPacket(adj);
    }

    // Read one connection message for this session's channel.
    async pump() {
        if (this.broken) this._throwBroken();
        const t = this.t, ms = this.replyMs;
        if (ms > 0) t.setDeadline(ms);
        let m;
        try {
            m = await t.channelMessage(this.ch);
        } catch (e) {
            if (ms > 0) t.setDeadline(0);
            if (e && typeof e === "object" && e.code === "deadline") await this.abandon();
            throw e;
        }
        if (ms > 0) t.setDeadline(0);
        if (m.type === "data") {
            if (m.data.length > 0) {
                this.chunks.push(m.data);
                this.chunked += m.data.length;
            }
            if (this.held() > sftp.MAX_PACKET + 4 + this.ch.recvInitial) {
                this.broken = "ssh.sftp: the server sent more than the window allows";
                this._throwBroken();
            }
        } else if (m.type === "close") {
            throw new Error("ssh.sftp: the channel closed mid-request");
        }
        await this.grant();
    }

    // Send one request and return ITS reply.
    async request(build, ...args) {
        const id = await this.submit(build, ...args);
        return (await this.collect([id]))[0];
    }

    // Send one request without waiting; returns its id.
    async submit(build, ...args) {
        this.id += 1;
        const id = this.id;
        await this.send(build(id, ...args));
        return id;
    }

    // The replies to `ids`, in the order of `ids`, matched by id.
    async collect(ids) {
        const want = new Set(ids);
        const got = new Map();
        let left = ids.length;
        while (left > 0) {
            const r = await this.recv();
            if (!want.has(r.id) || got.has(r.id)) {
                throw new Error("ssh.sftp: reply for request " + String(r.id)
                    + " while waiting for " + ids.join(","));
            }
            got.set(r.id, r);
            left -= 1;
        }
        return ids.map((id) => got.get(id));
    }

    // The next reply, which must answer one of the requests in `pending` (a
    // Set of ids).
    async recvPending(pending) {
        const r = await this.recv();
        if (!pending.has(r.id)) {
            const ids = Array.from(pending).sort((a, b) => a - b);
            throw new Error("ssh.sftp: reply for request " + String(r.id)
                + " while waiting for " + ids.join(","));
        }
        pending.delete(r.id);
        return r;
    }

    // Close a handle and collect the server's answer, on every path.
    async closeHandle(handle) {
        return this.request(sftp.buildClose, handle);
    }

    // Pull one SFTP message, feeding the channel as data arrives.
    async recv() {
        for (let n = 0; n < 100000; n++) {
            if (this.chunked > 0) {
                this.buf = this.buf + this.chunks.join("");
                this.chunks = [];
                this.chunked = 0;
            }
            const [p, used] = sftp.parseFrame(this.buf);
            if (p !== null && p !== undefined) {
                this.buf = this.buf.slice(used);
                await this.grant();
                return sftp.parse(p);
            }
            const need = this.buf.length < 4 ? 4 : wire.peekUint32(this.buf) + 4;
            while (this.held() < need) await this.pump();
        }
        throw new Error("ssh.sftp: no response");
    }

    async realpath(path) {
        const r = await this.request(sftp.buildRealpath, path);
        if (r.type !== "name") return [null, sftpError(r)];
        return [r.names[0] ? r.names[0].filename : null];
    }

    async stat(path) {
        return attrsResult(await this.request(sftp.buildStat, path));
    }

    async lstat(path) {
        return attrsResult(await this.request(sftp.buildLstat, path));
    }

    async mkdir(path, mode) {
        const attrs = (mode !== undefined && mode !== null)
            ? { permissions: modeArg(mode) } : null;
        return statusResult(await this.request(sftp.buildMkdir, path, attrs));
    }

    async rmdir(path) {
        return statusResult(await this.request(sftp.buildRmdir, path));
    }

    async remove(path) {
        return statusResult(await this.request(sftp.buildRemove, path));
    }

    async rename(from, to) {
        return statusResult(await this.request(sftp.buildRename, from, to));
    }

    async chmod(path, mode) {
        return this.setstat(path, { permissions: modeArg(mode) });
    }

    async setstat(path, attrs) {
        if (attrs === null || typeof attrs !== "object") {
            badArgument("ssh.sftp: setstat takes an attributes table");
        }
        return statusResult(await this.request(sftp.buildSetstat, path, attrs));
    }

    // List a directory -> [safe entries, refused] | [null, reason].
    async list(path) {
        const h = await this.request(sftp.buildOpendir, path);
        if (h.type !== "handle") return [null, sftpError(h)];
        const all = [];
        for (;;) {
            if (all.length > MAX_LIST_ENTRIES) {
                await this.closeHandle(h.handle);
                return [null, { code: "too_large", limit: MAX_LIST_ENTRIES,
                    detail: "directory has more than " + MAX_LIST_ENTRIES + " entries" }];
            }
            const r = await this.request(sftp.buildReaddir, h.handle);
            if (r.type === "status") {
                if (!r.eof) {
                    await this.closeHandle(h.handle);
                    return [null, sftpError(r)];
                }
                break;
            }
            if (r.type !== "name") {
                await this.closeHandle(h.handle);
                return [null, sftpError(r)];
            }
            if (r.names.length === 0) {
                await this.closeHandle(h.handle);
                return [null, { code: "bad_reply",
                    detail: "READDIR returned no names and no EOF" }];
            }
            for (const e of r.names) all.push(e);
        }
        await this.closeHandle(h.handle);
        return sftp.safeNames(all);
    }

    // Open a file -> [File] | [null, reason].
    async open(path, mode, opts) {
        const pflags = OPEN_MODES[mode === undefined || mode === null ? "r" : mode];
        if (!pflags || !Object.prototype.hasOwnProperty.call(OPEN_MODES, mode || "r")) {
            badArgument("ssh.sftp: open mode must be r, r+, w, a or wx");
        }
        const attrs = (opts && opts.mode !== undefined && opts.mode !== null)
            ? { permissions: modeArg(opts.mode) } : null;
        const h = await this.request(sftp.buildOpen, path, pflags, attrs);
        if (h.type !== "handle") return [null, sftpError(h)];
        return [new File(this, h.handle, mode === "a")];
    }

    // Read a whole file -> [bytes] | [null, reason]. `max` (default 16 MiB).
    async read(path, max) {
        if (max === undefined || max === null) max = 16 * 1024 * 1024;
        const [f, err] = await this.open(path, "r");
        if (!f) return [null, err];
        const parts = [];
        let total = 0;
        for (;;) {
            const [chunk, rerr] = await f.read(Math.min(WHOLE_READ, max + 1));
            if (chunk === null || chunk === undefined) {
                await f.close();
                return [null, rerr];
            }
            if (chunk === "") break;
            total += chunk.length;
            if (total > max) {
                await f.close();
                return [null, { code: "too_large", limit: max,
                    detail: "file exceeds the " + String(max) + " byte limit" }];
            }
            parts.push(chunk);
        }
        await f.close();
        return [parts.join("")];
    }

    // Write a whole file, creating or truncating it -> [true] | [null, reason].
    async write(path, data) {
        const [f, err] = await this.open(path, "w");
        if (!f) return [null, err];
        const [ok, werr] = await f.write(data);
        if (!ok) {
            await f.close();
            return [null, werr];
        }
        return f.close();
    }

    async close() {
        await this.t.closeChannel(this.ch);
    }

    // The server stopped answering this session: close its channel under a
    // short bound, and fail this call and every later one on the session.
    async abandon() {
        const t = this.t;
        this.broken = sshError("timeout", "no reply from the sftp server within "
            + String(this.replyMs) + " ms; the session is closed");
        t.setDeadline(ABANDON_MS);
        await t.closeChannel(this.ch);
        t.setDeadline(0);
        if (!this.ch.closed) {
            t.dead = { code: "timeout", detail: "a timed-out sftp session's channel did not close" };
            t.close();
        }
        throw this.broken;
    }
}

// One status reply as a result: [true], or [null, coded error].
function statusResult(r) {
    if (r.type === "status" && r.ok) return [true];
    return [null, sftpError(r)];
}

// Attributes as a caller sees them, from a decoded ATTRS reply.
function attrsResult(r) {
    if (r.type !== "attrs") return [null, sftpError(r)];
    const a = r.attrs;
    return [{ size: a.size, uid: a.uid, gid: a.gid, permissions: a.permissions,
              atime: a.atime, mtime: a.mtime, isDir: a.isDir }];
}

// A permission mode: 0o755 / 493, or the octal string "755".
function modeArg(mode) {
    if (typeof mode === "string" && /^[0-7]{3,4}$/.test(mode)) {
        return parseInt(mode, 8);
    }
    if (isInt(mode) && mode >= 0 && mode <= 0xFFF) return mode;
    badArgument("ssh.sftp: mode must be an octal string such as \"755\", or an integer");
}

const MAX_LIST_ENTRIES = 100000;

// One read or write request; requests in flight at once; Sftp.read's piece.
const IO_CHUNK = 32768;
const PIPELINE = 64;
const WHOLE_READ = 8 * 1024 * 1024;

const OPEN_MODES = {
    "r": sftp.FXF_READ,
    "r+": sftp.FXF_READ | sftp.FXF_WRITE,
    "w": sftp.FXF_WRITE | sftp.FXF_CREAT | sftp.FXF_TRUNC,
    "a": sftp.FXF_WRITE | sftp.FXF_CREAT | sftp.FXF_APPEND,
    "wx": sftp.FXF_WRITE | sftp.FXF_CREAT | sftp.FXF_EXCL,
};

function closedError() {
    return { code: "closed", detail: "the file is closed" };
}

class File {
    constructor(s, handle, append) {
        this.s = s;
        this.handle = handle;
        this.pos = 0;
        this.append = append;
        this.ramp = undefined;
    }

    // Read up to `n` bytes (default 32 KiB) -> [bytes] ("" at end of file)
    // | [null, reason].
    async read(n) {
        if (!this.handle) return [null, closedError()];
        if (n === undefined || n === null) n = IO_CHUNK;
        if (!isInt(n) || n < 1) {
            badArgument("ssh.sftp: read size must be a positive integer");
        }
        if (n > Number.MAX_SAFE_INTEGER - this.pos) {
            badArgument("ssh.sftp: read size too large");
        }
        const limit = this.pos + n;
        const pending = new Set(), requests = new Map();
        const ready = new Map();
        let count = 0;
        let nextOff = this.pos;
        let cursor = this.pos;
        const parts = [];
        let stop = false;
        let err = null;
        let depth = this.ramp || 1;

        for (;;) {
            while (!stop && count < depth && nextOff < limit) {
                const want = Math.min(IO_CHUNK, limit - nextOff);
                const id = await this.s.submit(sftp.buildRead, this.handle, nextOff, want);
                pending.add(id);
                requests.set(id, { off: nextOff, want });
                count += 1;
                nextOff += want;
            }
            if (count === 0) break;

            let r = await this.s.recvPending(pending);
            const req = requests.get(r.id);
            requests.delete(r.id);
            count -= 1;
            if (r.type === "data" && r.data.length > req.want) {
                r = { type: "overlong" };
            }
            if (r.type === "data" && r.data.length === req.want) {
                depth = Math.min(PIPELINE, depth + 1);
            } else {
                stop = true;
            }
            ready.set(req.off, { r, want: req.want });

            while (!err && ready.has(cursor)) {
                const e = ready.get(cursor);
                ready.delete(cursor);
                if (e.r.type === "data") {
                    parts.push(e.r.data);
                    cursor += e.r.data.length;
                    if (e.r.data.length < e.want) cursor = Infinity;
                } else if (e.r.type === "status" && e.r.eof) {
                    cursor = Infinity;
                } else if (e.r.type === "overlong") {
                    err = { code: "bad_reply",
                        detail: "READ returned more data than was asked for" };
                } else {
                    err = sftpError(e.r);
                }
            }
            if (err) stop = true;
        }

        this.ramp = depth;
        const out = parts.join("");
        this.pos += out.length;
        if (err) return [null, err];
        return [out];
    }

    // Write all of `data` (a byte string) -> [true] | [null, reason].
    async write(data) {
        if (!this.handle) return [null, closedError()];
        if (typeof data !== "string") {
            badArgument("ssh.sftp: write takes a string");
        }
        const pending = new Set();
        let count = 0, off = 0, err = null;
        for (;;) {
            while (!err && count < PIPELINE && off < data.length) {
                const chunk = data.slice(off, off + IO_CHUNK);
                pending.add(await this.s.submit(sftp.buildWrite, this.handle,
                                                this.pos + off, chunk));
                count += 1;
                off += chunk.length;
            }
            if (count === 0) break;
            const r = await this.s.recvPending(pending);
            count -= 1;
            if (!err && !(r.type === "status" && r.ok)) err = sftpError(r);
        }
        if (err) return [null, err];
        this.pos += data.length;
        return [true];
    }

    // Move the position to `offset` bytes from the start.
    seek(offset) {
        if (!isInt(offset) || offset < 0) {
            badArgument("ssh.sftp: seek takes a non-negative integer offset");
        }
        this.pos = offset;
        return true;
    }

    tell() { return this.pos; }

    async stat() {
        if (!this.handle) return [null, closedError()];
        return attrsResult(await this.s.request(sftp.buildFstat, this.handle));
    }

    // Close the file -> [true] | [null, reason]. Closing twice is harmless.
    async close() {
        if (!this.handle) return [true];
        const h = this.handle;
        this.handle = null;
        return statusResult(await this.s.closeHandle(h));
    }
}

// Open an SFTP session on its own channel of connection `t`
// -> [Sftp] | [null, reason].
async function open(t, opts) {
    let replyMs = opts ? opts.replyTimeoutMs : undefined;
    if (replyMs === undefined || replyMs === null) replyMs = REPLY_TIMEOUT_MS;
    if (!isInt(replyMs) || replyMs < 0 || replyMs > 86400000) {
        badArgument("ssh.sftp: replyTimeoutMs must be an integer 0..86400000");
    }
    const [ch, cerr] = await t.openSession();
    if (!ch) return [null, cerr];

    const fail = async (e) => {
        await t.closeChannel(ch);
        return [null, e];
    };

    await t.sendPacket(channel.buildSubsystem(ch.remoteId, "sftp"));
    let replied;
    try {
        replied = await t.awaitChannelReply(ch);
    } catch (e) {
        await t.closeChannel(ch);
        throw e;
    }
    if (!replied) return fail({ code: "sftp_unavailable" });

    const s = new Sftp(t, ch, replyMs);
    let ver;
    try {
        await s.send(sftp.buildInit());
        ver = await s.recv();
    } catch (e) {
        await t.closeChannel(ch);
        throw e;
    }
    if (ver.type !== "version") return fail({ code: "sftp_no_version" });
    s.version = ver.version;
    return [s];
}

export const sftpClient = { open, REPLY_TIMEOUT_MS, MAX_LIST_ENTRIES };
export default sftpClient;
