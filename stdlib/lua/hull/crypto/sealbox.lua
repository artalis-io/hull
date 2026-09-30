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
-- Whether a context was used is not itself recorded (TOTP's rows predate
-- contexts and must keep opening), so give a keyring one use: either always
-- with a context (hull.kv) or never (TOTP), not both.
--
-- Keys are 32 random bytes, grouped in a keyring:
--
--     local ring = sealbox.keyring{ keys = { [1] = k1, [2] = k2 }, current = 2 }
--
-- New seals use `current`; a blob opens under whichever key its version
-- names, which is what makes rotation a matter of adding a key.

local crypto = require('hull.crypto')

local M = {}

M.VERSION_LEN = 4
M.NONCE_LEN   = 24
M.MAC_LEN     = 16
M.MIN_LEN     = M.VERSION_LEN + M.NONCE_LEN + M.MAC_LEN

local spack, sunpack = string.pack, string.unpack

-- A key held in C (crypto.key_from_env): the bytes never enter the script
-- heap, and the handle seals and opens with them itself. Recognised by the
-- metatable the binding locks to this name.
local function is_key_handle(k)
    return type(k) == "userdata" and getmetatable(k) == "crypto.key"
end

local function box(key, msg, nonce)
    if is_key_handle(key) then return key:secretbox(msg, nonce) end
    return crypto.secretbox(msg, nonce, key)
end

local function unbox(key, ct, nonce)
    if is_key_handle(key) then return key:secretbox_open(ct, nonce) end
    return crypto.secretbox_open(ct, nonce, key)
end

--- A keyring from { keys = { [id] = 32-byte key, ... }, current = id }. A key
--- is a 32-byte string, or a handle from crypto.key_from_env, which keeps the
--- bytes in C (see keyring_from_env).
--- Ids are unsigned 32-bit integers; `current` must be one of them. Raises on
--- anything else: a keyring is configuration, and a wrong one is a bug to hear
--- about at startup rather than a value that later fails to open.
-- A key id: an integer 0..2^32-1, or the same written in canonical decimal
-- ("7", never "07", " 7" or "0x7") - ids often arrive from the environment as
-- text. nil for anything else. The JS module accepts exactly the same.
local function key_id(v)
    if type(v) == "string" then
        if not (v == "0" or v:match("^[1-9]%d*$")) or #v > 10 then return nil end
        v = math.tointeger(tonumber(v))
    end
    if math.type(v) ~= "integer" or v < 0 or v > 0xFFFFFFFF then return nil end
    return v
end

function M.keyring(opts)
    if type(opts) ~= "table" or type(opts.keys) ~= "table" then
        error("sealbox.keyring: expected { keys = {[id] = key, ...}, current = id }", 2)
    end
    local keys = {}
    for raw, k in pairs(opts.keys) do
        local id = key_id(raw)
        if not id then
            error("sealbox.keyring: key ids must be integers 0..2^32-1", 2)
        end
        if keys[id] then
            error("sealbox.keyring: key id " .. id .. " given twice", 2)
        end
        if not is_key_handle(k) and (type(k) ~= "string" or #k ~= 32) then
            error("sealbox.keyring: key " .. tostring(id)
                  .. " must be exactly 32 bytes or a crypto.key_from_env key", 2)
        end
        keys[id] = k
    end
    local current = key_id(opts.current)
    if current == nil or keys[current] == nil then
        error("sealbox.keyring: current key id " .. tostring(opts.current)
              .. " is not in keys", 2)
    end
    return { keys = keys, current = current }
end

--- A keyring whose keys are read from environment variables into memory the
--- C layer owns, so no key is ever a Lua string:
---
---   local ring = sealbox.keyring_from_env{
---       keys = { [1] = "KV_KEY_1", [2] = "KV_KEY_2" }, current = 2 }
---
--- Each variable holds 64 hex digits or base64 of 32 bytes, and must be in
--- manifest.env. hull/kv's `encrypt` and TOTP take the result, or its keys,
--- wherever they take a keyring.
function M.keyring_from_env(opts)
    if type(opts) ~= "table" or type(opts.keys) ~= "table" then
        error("sealbox.keyring_from_env: expected { keys = {[id] = \"VAR\", ...}, current = id }", 2)
    end
    local keys = {}
    for id, var in pairs(opts.keys) do
        if type(var) ~= "string" or var == "" then
            error("sealbox.keyring_from_env: key " .. tostring(id)
                  .. " must name an environment variable", 2)
        end
        keys[id] = crypto.key_from_env(var)
    end
    return M.keyring{ keys = keys, current = opts.current }
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
-- Arguments are checked, not coerced: sealing the wrong thing would produce a
-- blob that opens to something the caller never stored.
local function check_context(fname, context)
    if context == nil then return end
    if type(context) ~= "table" then
        error("sealbox." .. fname .. ": context must be an array of strings", 3)
    end
    for i = 1, #context do
        if type(context[i]) ~= "string" then
            error("sealbox." .. fname .. ": context entries must be strings", 3)
        end
    end
end

function M.seal(ring, value, context)
    if type(value) ~= "string" then error("sealbox.seal: value must be a string", 2) end
    check_context("seal", context)
    local key = ring.keys[ring.current]
    local nonce = crypto.random(M.NONCE_LEN)
    local ct = box(key, frame(context, value), nonce)
    return spack(">I4", ring.current) .. nonce .. ct
end

--- Open a blob sealed with `context`. Returns value, version on success;
--- otherwise nil and
---   "unknown_version"  the blob names a key this ring does not hold
---   "open_failed"      anything else: altered, forged, sealed for another
---                      context, or not a sealed blob at all
--- One reason covers every way a blob can fail to be genuine, on purpose, so
--- the answer tells an attacker nothing about which check stopped them.
function M.open(ring, blob, context)
    check_context("open", context)
    if type(blob) ~= "string" or #blob < M.MIN_LEN then return nil, "open_failed" end
    local version = sunpack(">I4", blob)
    local key = ring.keys[version]
    if not key then return nil, "unknown_version" end
    local nonce = blob:sub(M.VERSION_LEN + 1, M.VERSION_LEN + M.NONCE_LEN)
    local ct    = blob:sub(M.VERSION_LEN + M.NONCE_LEN + 1)
    local plain = unbox(key, ct, nonce)
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
    return unbox(key, blob:sub(M.NONCE_LEN + 1), blob:sub(1, M.NONCE_LEN))
end

return M
