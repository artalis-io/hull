-- hull.crypto.sealbox - versioned, authenticated sealing of values at rest.
--
-- One implementation of the part that is easy to get subtly wrong, shared by
-- everything in the stdlib that keeps a secret in storage: hull.kv's
-- encrypted handles and hull/web/middleware/totp. Design and threat model:
-- docs/kv_encryption_design.md.
--
-- A sealed blob is
--
--     version  u32 BE     the id of the key that sealed it
--     nonce    24 bytes   random, per seal
--     box      NaCl secretbox (XSalsa20-Poly1305) of the frame
--
-- and the frame is the value itself, or - when a CONTEXT is given - each
-- context string length-prefixed, then the value. secretbox has no
-- associated-data input, so the context goes inside the box and is checked on
-- open: a value sealed for one key name cannot be moved to another and still
-- open. The JS module (hull:crypto:sealbox) produces the same bytes.
--
-- Keys are 32 random bytes, grouped in a keyring:
--
--     local ring = sealbox.keyring{ keys = { [1] = k1, [2] = k2 }, current = 2 }
--
-- New seals use `current`; a blob opens under whichever key its version
-- names, which is what makes rotation a matter of adding a key.

local crypto = require('hull.crypto')
local hex    = require('hull.encoding').hex

local M = {}

M.VERSION_LEN = 4
M.NONCE_LEN   = 24
M.MAC_LEN     = 16
M.MIN_LEN     = M.VERSION_LEN + M.NONCE_LEN + M.MAC_LEN

local spack, sunpack = string.pack, string.unpack

--- A keyring from { keys = { [id] = 32-byte key, ... }, current = id }.
--- Ids are unsigned 32-bit integers; `current` must be one of them. Raises on
--- anything else: a keyring is configuration, and a wrong one is a bug to hear
--- about at startup rather than a value that later fails to open.
function M.keyring(opts)
    if type(opts) ~= "table" or type(opts.keys) ~= "table" then
        error("sealbox.keyring: expected { keys = {[id] = key, ...}, current = id }", 2)
    end
    local keys = {}
    for id, k in pairs(opts.keys) do
        if math.type(id) ~= "integer" or id < 0 or id > 0xFFFFFFFF then
            error("sealbox.keyring: key ids must be integers 0..2^32-1", 2)
        end
        if type(k) ~= "string" or #k ~= 32 then
            error("sealbox.keyring: key " .. tostring(id) .. " must be exactly 32 bytes", 2)
        end
        keys[id] = hex.encode(k)
    end
    if keys[opts.current] == nil then
        error("sealbox.keyring: current key id " .. tostring(opts.current)
              .. " is not in keys", 2)
    end
    return { keys = keys, current = opts.current }
end

-- The frame: the value, preceded by each context string, length-prefixed so
-- no boundary is ambiguous ("ab" + "c" is not "a" + "bc").
local function frame(context, value)
    if not context then return value end
    local parts = {}
    for i, c in ipairs(context) do parts[i] = spack(">s4", c) end
    parts[#parts + 1] = value
    return table.concat(parts)
end

-- The value inside a frame, or nil when its context is not `context`.
local function unframe(context, plain)
    if not context then return plain end
    local pos = 1
    for _, want in ipairs(context) do
        local ok, got, nxt = pcall(sunpack, ">s4", plain, pos)
        if not ok or got ~= want then return nil end
        pos = nxt
    end
    return plain:sub(pos)
end

--- Seal `value` under the ring's current key, bound to `context` (an array of
--- strings, or nil). Returns the blob.
function M.seal(ring, value, context)
    local key = ring.keys[ring.current]
    local nonce = crypto.random(M.NONCE_LEN)
    local ct_hex = crypto.secretbox(frame(context, value),
                                    hex.encode(nonce), key)
    return spack(">I4", ring.current) .. nonce .. hex.decode(ct_hex)
end

--- Open a blob sealed with `context`. Returns value, version on success;
--- otherwise nil and
---   "unknown_version"  the blob names a key this ring does not hold
---   "open_failed"      anything else: altered, forged, sealed for another
---                      context, or not a sealed blob at all
--- One reason covers every way a blob can fail to be genuine, on purpose, so
--- the answer tells an attacker nothing about which check stopped them.
function M.open(ring, blob, context)
    if type(blob) ~= "string" or #blob < M.MIN_LEN then return nil, "open_failed" end
    local version = sunpack(">I4", blob)
    local key = ring.keys[version]
    if not key then return nil, "unknown_version" end
    local nonce = blob:sub(M.VERSION_LEN + 1, M.VERSION_LEN + M.NONCE_LEN)
    local ct    = blob:sub(M.VERSION_LEN + M.NONCE_LEN + 1)
    local plain = crypto.secretbox_open(hex.encode(ct),
                                        hex.encode(nonce), key)
    if not plain then return nil, "open_failed" end
    local value = unframe(context, plain)
    if not value then return nil, "open_failed" end
    return value, version
end

--- Open a blob in the older unversioned shape, nonce(24) || box, with key
--- `id`. Only TOTP has rows like this (from before key rotation); nothing new
--- is ever sealed this way. Returns the value, or nil.
function M.open_unversioned(ring, id, blob)
    local key = ring.keys[id]
    if not key or type(blob) ~= "string" or #blob < M.NONCE_LEN + M.MAC_LEN then
        return nil
    end
    return crypto.secretbox_open(hex.encode(blob:sub(M.NONCE_LEN + 1)),
                                 hex.encode(blob:sub(1, M.NONCE_LEN)), key)
end

return M
