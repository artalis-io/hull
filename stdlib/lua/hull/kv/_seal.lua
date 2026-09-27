--[[
  hull.kv._seal - the encryption layer of an encrypted kv handle.

  Built by hull.kv.open{ encrypt = ... } and consulted by the handle's value
  methods; a plain handle (and every hull.cache handle) has none. Each value is
  sealed with hull.crypto.sealbox under the handle's keyring, bound to the
  namespace and the key name, so a value copied to another key or namespace no
  longer opens. Design: docs/kv_encryption_design.md.

  Internal module. SPDX-License-Identifier: AGPL-3.0-or-later
]]

local u       = require("hull.kv._util")
local sealbox = require("hull.crypto.sealbox")

local M = {}
local S = {}
S.__index = S

--- From the `encrypt` option: { keys = { [id] = 32 bytes }, current = id,
--- allow_plaintext = bool }. Raises invalid_argument on a bad keyring.
function M.new(encrypt, namespace)
    if type(encrypt) ~= "table" then
        u.error("invalid_argument",
            "kv.open: encrypt must be { keys = {[id] = key}, current = id }")
    end
    local ok, ring = pcall(sealbox.keyring, encrypt)
    if not ok then u.error("invalid_argument", "kv.open: " .. tostring(ring)) end
    return setmetatable({ ring = ring, namespace = namespace,
                          allow_plaintext = encrypt.allow_plaintext == true }, S)
end

--- The stored form of `value` under key `k`.
function S:seal(k, value)
    return sealbox.seal(self.ring, value, { self.namespace, k })
end

--- The value stored under `k`, and the id of the key that sealed it (nil for
--- a plaintext value read under allow_plaintext). Raises decrypt_failed.
function S:open(k, stored)
    local value, version = sealbox.open(self.ring, stored, { self.namespace, k })
    if value ~= nil then return value, version end
    if self.allow_plaintext then
        -- Migration only: a value that does not open is taken as written
        -- before encryption was turned on. While this is set, a plaintext
        -- value planted by a writer is accepted too - the docs say so.
        return stored, nil
    end
    u.error("decrypt_failed", "kv: the value for this key does not open with "
            .. "the handle's keys (altered, from another key or namespace, "
            .. "or sealed with a key not in the keyring)")
end

--- Whether a stored value is sealed under the current key already.
function S:is_current(version) return version == self.ring.current end

return M
