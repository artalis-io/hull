-- hull.ssh.known_hosts - OpenSSH's known_hosts format, as text <-> entries.
--
-- Pure: no files, no I/O. hull.ssh's file_store reads and writes the file and
-- uses this to understand it, so a trust store Hull keeps is one the `ssh`
-- command line reads, and the reverse - including output from ssh-keyscan.
--
-- A line is
--
--     hosts keytype base64-key [comment]
--
-- where `hosts` is a comma list of names - "host", or "[host]:port" off port
-- 22, which is exactly how hull.ssh.hostkey.store_name spells them - or one
-- HASHED name, "|1|base64(salt)|base64(HMAC-SHA1(salt, name))", written by
-- `ssh -o HashKnownHosts=yes`.
--
-- Deliberately not supported, and skipped rather than half-honoured:
--   * patterns ("*.example.com", "!bastion") - a name matches only itself, so
--     a host covered only by a pattern is simply unknown, never wrongly
--     trusted;
--   * @cert-authority and @revoked lines - certificates are not implemented;
--     a key listed only as revoked is not trusted either, since it is not
--     listed as a host key.

local base64 = require('hull.encoding.base64')
local wire   = require('hull.ssh.wire')

local M = {}

M.HASH_MAGIC = "|1|"

-- The key type a wire-format key blob declares: its first string.
function M.blob_type(blob)
    local ok, t = pcall(function() return wire.reader(blob):string() end)
    return ok and t or nil
end

-- One line, or nil when it holds no usable entry (blank, comment, marker,
-- malformed). Never raises: a file edited by hand is not a reason to fail a
-- connection, and a line Hull cannot read is a line it does not trust.
function M.parse_line(line)
    local s = line:gsub("^%s+", ""):gsub("%s+$", "")
    if s == "" or s:sub(1, 1) == "#" or s:sub(1, 1) == "@" then return nil end

    local hosts, keytype, b64 = s:match("^(%S+)%s+(%S+)%s+(%S+)")
    if not hosts then return nil end
    local ok, blob = pcall(base64.decode, b64)
    if not ok or blob == "" or M.blob_type(blob) ~= keytype then return nil end

    local e = { keytype = keytype, blob = blob }
    if hosts:sub(1, #M.HASH_MAGIC) == M.HASH_MAGIC then
        local salt64, hash64 = hosts:match("^|1|([^|]+)|([^|]+)$")
        if not salt64 then return nil end
        local sok, salt = pcall(base64.decode, salt64)
        local hok, hash = pcall(base64.decode, hash64)
        if not (sok and hok) or #hash ~= 20 then return nil end
        e.salt, e.hash = salt, hash
    else
        e.names = {}
        for name in hosts:gmatch("[^,]+") do
            -- Patterns are skipped (see the header); plain names compare
            -- case-insensitively, as DNS names do.
            if not name:find("[%*%?!]") then e.names[#e.names + 1] = name:lower() end
        end
        if #e.names == 0 then return nil end
    end
    return e
end

-- Whether entry `e` is for store name `name`. `hmac_sha1(key, msg)` returns the
-- raw 20-byte digest; it is only called for hashed entries.
function M.matches(e, name, hmac_sha1)
    if e.names then
        for _, n in ipairs(e.names) do
            if n == name then return true end
        end
        return false
    end
    return hmac_sha1(e.salt, name) == e.hash
end

-- The line recording `blob` for `name`: plain, or hashed with `salt` (16 random
-- bytes) when one is given.
function M.render(name, blob, salt, hmac_sha1)
    local keytype = M.blob_type(blob)
    if not keytype then error("ssh.known_hosts: not a key blob", 2) end
    local hosts = name
    if salt then
        hosts = M.HASH_MAGIC .. base64.encode(salt) .. "|"
                .. base64.encode(hmac_sha1(salt, name))
    end
    return hosts .. " " .. keytype .. " " .. base64.encode(blob)
end

-- `text` without the entries for `name`. A plain line listing several names
-- keeps the others; every other line is kept exactly as it was.
function M.without(text, name, hmac_sha1)
    local out = {}
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do
        local e = M.parse_line(line)
        if e and M.matches(e, name, hmac_sha1) then
            if e.names and #e.names > 1 then
                local hosts, rest = line:match("^%s*(%S+)(.*)$")
                local kept = {}
                for n in hosts:gmatch("[^,]+") do
                    if n:lower() ~= name then kept[#kept + 1] = n end
                end
                if #kept > 0 then out[#out + 1] = table.concat(kept, ",") .. rest end
            end
        else
            out[#out + 1] = line
        end
    end
    -- The split above adds one empty line at the end; do not grow the file.
    if out[#out] == "" then out[#out] = nil end
    return table.concat(out, "\n") .. (#out > 0 and "\n" or "")
end

return M
