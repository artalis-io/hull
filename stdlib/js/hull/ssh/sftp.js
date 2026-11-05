// hull:ssh:sftp - the SFTP subsystem protocol, version 3.
//
// The JS twin of hull.ssh.sftp (stdlib/lua/hull/ssh/sftp.lua); see there for
// the reasoning. A path travels as a length-prefixed string inside a binary
// protocol, never a shell word. Names that arrive FROM a server are what
// needs defending: see unsafeName.
//
// No I/O: builders, parsers, and the framing.

import { wire } from "hull:ssh:wire";

const VERSION = 3;

// requests
const FXP_INIT     = 1;
const FXP_VERSION  = 2;
const FXP_OPEN     = 3;
const FXP_CLOSE    = 4;
const FXP_READ     = 5;
const FXP_WRITE    = 6;
const FXP_LSTAT    = 7;
const FXP_FSTAT    = 8;
const FXP_SETSTAT  = 9;
const FXP_OPENDIR  = 11;
const FXP_READDIR  = 12;
const FXP_REMOVE   = 13;
const FXP_MKDIR    = 14;
const FXP_RMDIR    = 15;
const FXP_REALPATH = 16;
const FXP_STAT     = 17;
const FXP_RENAME   = 18;

// responses
const FXP_STATUS = 101;
const FXP_HANDLE = 102;
const FXP_DATA   = 103;
const FXP_NAME   = 104;
const FXP_ATTRS  = 105;

// status codes
const FX_OK                = 0;
const FX_EOF               = 1;
const FX_NO_SUCH_FILE      = 2;
const FX_PERMISSION_DENIED = 3;
const FX_FAILURE           = 4;
const FX_BAD_MESSAGE       = 5;
const FX_NO_CONNECTION     = 6;
const FX_CONNECTION_LOST   = 7;
const FX_OP_UNSUPPORTED    = 8;

const STATUS_TEXT = {
    0: "ok", 1: "end of file", 2: "no such file",
    3: "permission denied", 4: "failure", 5: "bad message",
    6: "no connection", 7: "connection lost", 8: "operation unsupported",
};

// The same statuses as codes a caller branches on (`err.code`).
const STATUS_CODE = {
    0: "ok", 1: "eof", 2: "no_such_file", 3: "permission_denied",
    4: "failure", 5: "bad_message", 6: "no_connection",
    7: "connection_lost", 8: "op_unsupported",
};

const hasOwn = Object.prototype.hasOwnProperty;
const lookup = (t, k) => (hasOwn.call(t, k) ? t[k] : undefined);

// open flags
const FXF_READ   = 0x01;
const FXF_WRITE  = 0x02;
const FXF_APPEND = 0x04;
const FXF_CREAT  = 0x08;
const FXF_TRUNC  = 0x10;
const FXF_EXCL   = 0x20;

// attribute flags
const ATTR_SIZE        = 0x01;
const ATTR_UIDGID      = 0x02;
const ATTR_PERMISSIONS = 0x04;
const ATTR_ACMODTIME   = 0x08;
const ATTR_EXTENDED    = 0x80000000;

// A single SFTP packet is bounded well below the SSH packet cap.
const MAX_PACKET = 256 * 1024;

// A Lua truthy value: present and not false (0 counts, as in Lua).
const has = (v) => v !== undefined && v !== null && v !== false;

// Framing --------------------------------------------------------------------

// SFTP has its own length prefix inside the channel data stream.
function frame(payload) {
    if (payload.length > MAX_PACKET) {
        throw new RangeError("ssh.sftp: packet too large to send");
    }
    return wire.uint32(payload.length) + payload;
}

// Pull one packet off the front of `buf`.
// -> [payload, consumed] | [null, "need_more"]; throws on a length that can
// never be valid.
function parseFrame(buf) {
    if (buf.length < 4) return [null, "need_more"];
    const n = wire.peekUint32(buf);
    // Bounded BEFORE waiting for the bytes.
    if (n > MAX_PACKET) {
        throw new Error("ssh.sftp: declared packet length " + String(n)
            + " exceeds the maximum");
    }
    if (n < 1) throw new Error("ssh.sftp: packet length below the minimum");
    if (buf.length < n + 4) return [null, "need_more"];
    return [buf.slice(4, 4 + n), n + 4];
}

// Attributes -----------------------------------------------------------------

// Encode an attribute block; nothing yields the empty block.
function encodeAttrs(a) {
    const w = wire.writer();
    if (!has(a)) return w.uint32(0).build();
    let flags = 0;
    if (has(a.size)) flags |= ATTR_SIZE;
    if (has(a.uid) && has(a.gid)) flags |= ATTR_UIDGID;
    if (has(a.permissions)) flags |= ATTR_PERMISSIONS;
    if (has(a.atime) && has(a.mtime)) flags |= ATTR_ACMODTIME;
    w.uint32(flags >>> 0);
    if (has(a.size)) w.uint64(a.size);
    if (has(a.uid) && has(a.gid)) w.uint32(a.uid).uint32(a.gid);
    if (has(a.permissions)) w.uint32(a.permissions);
    if (has(a.atime) && has(a.mtime)) w.uint32(a.atime).uint32(a.mtime);
    return w.build();
}

function decodeAttrs(r) {
    const flags = r.uint32();
    const a = { flags };
    if ((flags & ATTR_SIZE) !== 0) {
        // A size past 2^53 - 1 is no exact JS number: reported as null, not
        // thrown - one such entry failed a whole list() (#1009 review).
        const hi = r.uint32(), lo = r.uint32();
        const v = hi * 0x100000000 + lo;
        a.size = v <= Number.MAX_SAFE_INTEGER ? v : null;
    }
    if ((flags & ATTR_UIDGID) !== 0) { a.uid = r.uint32(); a.gid = r.uint32(); }
    if ((flags & ATTR_PERMISSIONS) !== 0) a.permissions = r.uint32();
    if ((flags & ATTR_ACMODTIME) !== 0) { a.atime = r.uint32(); a.mtime = r.uint32(); }
    if ((flags & ATTR_EXTENDED) !== 0) {
        const count = r.uint32();
        // The reader bounds-checks each string, so a dishonest count runs out
        // of buffer rather than looping forever.
        for (let i = 0; i < count; i++) { r.string(); r.string(); }
    }
    // A directory bit, for readdir listings.
    if (a.permissions !== undefined) a.isDir = (a.permissions & 0xF000) === 0x4000;
    return a;
}

// Requests -------------------------------------------------------------------

function buildInit(version) {
    return wire.writer().byte(FXP_INIT).uint32(has(version) ? version : VERSION).build();
}

function req(t, id) {
    return wire.writer().byte(t).uint32(id);
}

// The path is a string: never split, quoted, or interpreted.
function buildOpen(id, path, pflags, attrs) {
    return req(FXP_OPEN, id).string(path).uint32(pflags).raw(encodeAttrs(attrs)).build();
}

function buildClose(id, handle) { return req(FXP_CLOSE, id).string(handle).build(); }

function buildRead(id, handle, offset, len) {
    return req(FXP_READ, id).string(handle).uint64(offset).uint32(len).build();
}

function buildWrite(id, handle, offset, data) {
    return req(FXP_WRITE, id).string(handle).uint64(offset).string(data).build();
}

function buildStat(id, path)    { return req(FXP_STAT, id).string(path).build(); }
function buildLstat(id, path)   { return req(FXP_LSTAT, id).string(path).build(); }
function buildFstat(id, handle) { return req(FXP_FSTAT, id).string(handle).build(); }
function buildOpendir(id, path) { return req(FXP_OPENDIR, id).string(path).build(); }
function buildReaddir(id, h)    { return req(FXP_READDIR, id).string(h).build(); }
function buildRemove(id, path)  { return req(FXP_REMOVE, id).string(path).build(); }
function buildRmdir(id, path)   { return req(FXP_RMDIR, id).string(path).build(); }
function buildRealpath(id, p)   { return req(FXP_REALPATH, id).string(p).build(); }

function buildMkdir(id, path, attrs) {
    return req(FXP_MKDIR, id).string(path).raw(encodeAttrs(attrs)).build();
}

function buildRename(id, from, to) {
    return req(FXP_RENAME, id).string(from).string(to).build();
}

function buildSetstat(id, path, attrs) {
    return req(FXP_SETSTAT, id).string(path).raw(encodeAttrs(attrs)).build();
}

// Responses ------------------------------------------------------------------

function parse(payload) {
    const r = wire.reader(payload);
    const t = r.byte();

    if (t === FXP_VERSION) {
        const version = r.uint32();
        // Extensions follow; ignored rather than refused.
        return { type: "version", version };
    }

    const id = r.uint32();

    if (t === FXP_STATUS) {
        const code = r.uint32();
        const out = { type: "status", id, code,
                      name: lookup(STATUS_CODE, code) || "failure",
                      ok: code === FX_OK, eof: code === FX_EOF };
        // Version 3 carries a message and language; older servers may not.
        if (r.remaining() > 0) {
            out.message = r.string();
            if (r.remaining() > 0) out.language = r.string();
        }
        // `text` reaches a caller's error value: controls removed.
        out.text = out.message !== undefined ? wire.safeName(out.message, 200) : undefined;
        if (out.text === undefined || out.text === "") {
            out.text = lookup(STATUS_TEXT, code) || ("status " + String(code));
        }
        return out;
    }

    if (t === FXP_HANDLE) return { type: "handle", id, handle: r.string() };
    if (t === FXP_DATA) return { type: "data", id, data: r.string() };

    if (t === FXP_NAME) {
        const count = r.uint32();
        const names = [];
        for (let i = 0; i < count; i++) {
            // The filename stays exact; longname is cleaned display text.
            const filename = r.string();
            const longname = wire.safeName(r.string(), 1024);
            names.push({ filename, longname, attrs: decodeAttrs(r) });
        }
        return { type: "name", id, names };
    }

    if (t === FXP_ATTRS) return { type: "attrs", id, attrs: decodeAttrs(r) };

    throw new Error("ssh.sftp: unexpected response type " + String(t));
}

// Names from the server ------------------------------------------------------

// Whether a filename from a server reply is safe to join onto a local path.
// Does NOT sanitize. -> null when usable, or a reason.
function unsafeName(name) {
    if (typeof name !== "string" || name === "") return "empty name";
    if (name === "." || name === "..") return "special directory entry";
    if (name.indexOf("/") >= 0) return "contains a path separator";
    // A separator on the Windows host that may be receiving the file.
    if (name.indexOf("\\") >= 0) return "contains a backslash";
    if (name.indexOf("\x00") >= 0) return "contains a NUL";
    if (/[\x00-\x1f\x7f]/.test(name)) return "contains a control character";
    return null;
}

// Filter a READDIR reply to the safe entries, reporting the rest.
// -> [ok, refused]
function safeNames(names) {
    const ok = [], refused = [];
    for (const e of names) {
        const why = unsafeName(e.filename);
        if (why) refused.push({ filename: e.filename, reason: why });
        else ok.push(e);
    }
    return [ok, refused];
}

export const sftp = {
    VERSION,
    FXP_INIT, FXP_VERSION, FXP_OPEN, FXP_CLOSE, FXP_READ, FXP_WRITE, FXP_LSTAT,
    FXP_FSTAT, FXP_SETSTAT, FXP_OPENDIR, FXP_READDIR, FXP_REMOVE, FXP_MKDIR,
    FXP_RMDIR, FXP_REALPATH, FXP_STAT, FXP_RENAME,
    FXP_STATUS, FXP_HANDLE, FXP_DATA, FXP_NAME, FXP_ATTRS,
    FX_OK, FX_EOF, FX_NO_SUCH_FILE, FX_PERMISSION_DENIED, FX_FAILURE,
    FX_BAD_MESSAGE, FX_NO_CONNECTION, FX_CONNECTION_LOST, FX_OP_UNSUPPORTED,
    STATUS_TEXT, STATUS_CODE,
    FXF_READ, FXF_WRITE, FXF_APPEND, FXF_CREAT, FXF_TRUNC, FXF_EXCL,
    ATTR_SIZE, ATTR_UIDGID, ATTR_PERMISSIONS, ATTR_ACMODTIME, ATTR_EXTENDED,
    MAX_PACKET,
    frame, parseFrame, encodeAttrs, decodeAttrs,
    buildInit, buildOpen, buildClose, buildRead, buildWrite, buildStat,
    buildLstat, buildFstat, buildOpendir, buildReaddir, buildRemove,
    buildRmdir, buildRealpath, buildMkdir, buildRename, buildSetstat,
    parse, unsafeName, safeNames,
};
export default sftp;
