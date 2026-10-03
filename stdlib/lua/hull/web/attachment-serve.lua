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

local _text = require("hull._text")   -- linear trims (see hull._text)
local attachment = require("hull.attachment")
local blob = require("hull.blob")
local encoding = require("hull.encoding")

local M = {}

-- RFC 5987 attr-char set: ALPHA / DIGIT / !#$&+-.^_`|~ - the RFC 3986
-- unreserved set plus these. Anything else is percent-encoded.
local ATTR_CHAR = { keep = "!#$&+^`|" }

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
-- Percent-encoding works on bytes, which is what RFC 5987 wants: it
-- encodes the raw UTF-8 octets of the name.
local function content_disposition(name)
    local pct = encoding.url.encode(name, ATTR_CHAR)
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
            res:header("ETag", etag)
            res:status(304)
            return
        end
        for part in inm:gmatch("[^,]+") do
            local trimmed = _text.trim(part)
            if trimmed == etag then
                -- RFC 9110 15.4.5: a 304 carries the ETag the 200 would have.
                res:header("ETag", etag)
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
    -- The stored MIME type is whatever the uploader claimed. nosniff stops a
    -- browser second-guessing it - rendering an uploaded "text/plain" that
    -- looks like HTML as HTML, on this origin.
    res:header("X-Content-Type-Options", "nosniff")
    res:bytes(bytes)
end

return M
