--- Auth-gated HTTP response helper for hull/attachment.
--
-- @module hull.web.attachment-serve
-- @license AGPL-3.0-or-later
--
-- Thin web layer over @{hull.attachment}: takes an `(req, res, id, opts)`
-- and produces an auth-gated, ETagged, Content-Disposition-bearing
-- response body. The core attachment module is intentionally HTTP-free
-- (works in CLI tools too); this is the only piece coupled to
-- `hull/http-server` and `res:bytes`.
--
-- Design notes:
--
--   * **Default deny.** If `opts.auth_check` is not supplied, the
--     helper responds 403. Hull's other auth modules (session,
--     rbac) follow the same fail-closed pattern.
--   * **Strong ETag from blob_id.** Because the underlying blob
--     store is content-addressed by SHA-256, the blob_id IS a
--     genuine cryptographic fingerprint of the bytes - strong
--     ETags are correct here (no risk of two attachments with the
--     same id but different bytes). We use `"<full-64-hex>"`.
--   * **Content-Disposition (RFC 5987).** Browsers should save
--     uploads under the original filename even when it contains
--     non-ASCII. The wire format is:
--
--         attachment; filename="<ascii-fallback>"; filename*=UTF-8''<pct-encoded>
--
--     Old clients honour `filename=`; modern ones prefer `filename*=`.
--
-- API:
--
--   serve(req, res, id, opts)
--     opts.auth_check    function(req, meta) → bool   REQUIRED
--                        (omit → unconditional 403)

local attachment = require("hull.attachment")
local blob = require("hull.blob")

local M = {}

-- RFC 5987 attr-char set: ALPHA / DIGIT / !#$&+-.^_`|~
-- Anything else (including space, /, etc.) gets percent-encoded.
local function pct_encode_attr_char(c)
    local b = string.byte(c)
    if (b >= 0x30 and b <= 0x39)   -- 0-9
        or (b >= 0x41 and b <= 0x5A) -- A-Z
        or (b >= 0x61 and b <= 0x7A) -- a-z
        or b == 0x21 or b == 0x23 or b == 0x24 or b == 0x26 -- ! # $ &
        or b == 0x2B or b == 0x2D or b == 0x2E or b == 0x5E -- + - . ^
        or b == 0x5F or b == 0x60 or b == 0x7C or b == 0x7E -- _ ` | ~
    then
        return c
    end
    return string.format("%%%02X", b)
end

-- ASCII fallback: replace non-ASCII bytes with `_`; escape `"` and `\`
-- so the quoted-string can't break out of the header field.
local function ascii_fallback(name)
    local out = {}
    for i = 1, #name do
        local b = string.byte(name, i)
        if b == 0x22 or b == 0x5C then       -- " or \  → backslash-escape
            out[#out + 1] = "\\"
            out[#out + 1] = string.char(b)
        elseif b < 0x20 or b > 0x7E then     -- non-printable / non-ASCII
            out[#out + 1] = "_"
        else
            out[#out + 1] = string.char(b)
        end
    end
    return table.concat(out)
end

-- Build the full Content-Disposition header value with both the
-- ASCII fallback and the RFC 5987 percent-encoded UTF-8 form.
-- `gsub(".", ...)` is BYTE-WISE (Lua's `.` matches one byte, not one
-- UTF-8 codepoint) - exactly what RFC 5987 wants since it encodes
-- raw UTF-8 octets. The JS sibling has to UTF-8-encode the input
-- string first because JS strings are UTF-16.
local function content_disposition(name)
    local pct = name:gsub(".", pct_encode_attr_char)
    return string.format(
        'attachment; filename="%s"; filename*=UTF-8\'\'%s',
        ascii_fallback(name), pct)
end

--- Serve an attachment over HTTP.
--
-- @tparam table req
-- @tparam table res
-- @tparam string id
-- @tparam[opt] table opts
--   `auth_check` - `function(req, metadata) -> bool`. REQUIRED for
--     non-403 responses. Receives the live metadata row so the
--     check can do per-tenant / per-user gating. Omit to deny
--     unconditionally.
function M.serve(req, res, id, opts)
    opts = opts or {}

    local meta = attachment.metadata(id)
    if not meta then
        res:status(404):json({ error = "not found" })
        return
    end

    -- Default-deny: caller must explicitly supply auth_check AND it
    -- must return truthy. Missing function, false, or nil → 403.
    if type(opts.auth_check) ~= "function" or
       not opts.auth_check(req, meta) then
        res:status(403):json({ error = "forbidden" })
        return
    end

    -- Strong ETag from full blob_id SHA. Content-addressed dedup
    -- means this is a genuine cryptographic fingerprint of the bytes.
    local etag = '"' .. meta.blob_id .. '"'

    -- If-None-Match → 304. Accepts comma-separated values + `*` wildcard.
    local inm = req.headers and req.headers["if-none-match"]
    if inm then
        if inm:match("^%s*%*%s*$") then
            res:status(304)
            return
        end
        for part in inm:gmatch("[^,]+") do
            local trimmed = part:match("^%s*(.-)%s*$")
            if trimmed == etag then
                res:status(304)
                return
            end
        end
    end

    local bytes = blob.get(meta.blob_id)
    if not bytes then
        -- Metadata says it exists but the blob is missing. Treat as
        -- 410 Gone so caches know to drop their copy.
        res:status(410):json({ error = "blob missing" })
        return
    end

    res:header("Content-Type", meta.mime)
    res:header("Content-Disposition", content_disposition(meta.original_name))
    res:header("ETag", etag)
    res:bytes(bytes)
end

return M
