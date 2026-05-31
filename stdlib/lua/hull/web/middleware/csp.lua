--- Content-Security-Policy middleware factory with per-request nonce.
--
-- @module hull.web.middleware.csp
-- @license AGPL-3.0-or-later
--
-- Generates a fresh 128-bit nonce on every request, exposes it via
-- `req.ctx.csp_nonce`, and sets the `Content-Security-Policy` header
-- on the outbound response. Templates that need a nonce read it from
-- the context and emit it on `<script>` and `<style>` tags:
--
--     <script nonce="{{ csp_nonce }}">...</script>
--     <style nonce="{{ csp_nonce }}">...</style>
--
-- Three named profiles are provided. All three include the same
-- non-nonce directives (default-src 'self', img-src 'self' data:,
-- form-action 'self', frame-ancestors 'none', base-uri 'self') and
-- differ only in how strictly they treat inline styles:
--
-- * `csp.htmx()`. The Pico-compatible profile. Allows inline HTML
--   `style="…"` attributes via `style-src-attr 'unsafe-inline'`.
--   `<script>` and `<style>` blocks still require the per-request
--   nonce. Use this with Pico v2 classless (which uses inline styles
--   on some components: `<details>`, `<dialog>`, form controls).
-- * `csp.strict()`. The strictest profile. No `style-src-attr`
--   escape; inline styles are blocked entirely. Use when you control
--   every template + ship a CSS framework without inline styles.
-- * `csp.custom(opts)`. Build your own. See `opts` shape below.
--
-- All three middlewares set the header on the response and continue
-- (return 0). Failure modes are limited to `crypto.random` failure,
-- which is treated as a server error (return 1, 500 response).

local crypto = require("hull.crypto")

local csp = {}

-- Base64url-encode raw bytes for use as a CSP nonce attribute value.
-- 16 bytes (128 bits of entropy) -> 22 chars unpadded. The base64url
-- alphabet is HTML-attribute-safe. Strip trailing '='.
local function nonce_b64url(n_bytes)
    n_bytes = n_bytes or 16
    local raw = crypto.random(n_bytes)
    if not raw or #raw ~= n_bytes then return nil end
    local b64 = crypto.base64url_encode(raw)
    if not b64 then return nil end
    -- crypto.base64url_encode already strips padding in Hull's
    -- implementation; this is defensive in case it changes.
    return (b64:gsub("=+$", ""))
end

-- Build the CSP header value for a given profile + nonce.
local function build_header(profile, nonce)
    local nonce_token = "'nonce-" .. nonce .. "'"
    local parts = {
        "default-src 'self'",
        "script-src 'self' " .. nonce_token,
        "style-src 'self' " .. nonce_token,
        "img-src 'self' data:",
        "form-action 'self'",
        "frame-ancestors 'none'",
        "base-uri 'self'",
    }
    if profile == "htmx" then
        -- Pico v2 classless uses inline style="…" attributes on a few
        -- components. style-src-attr controls ONLY inline style
        -- attributes; <style> blocks still require the nonce above.
        table.insert(parts, "style-src-attr 'unsafe-inline'")
    end
    return table.concat(parts, "; ")
end

-- The middleware body. Shared by all profiles; differs only on
-- `profile` arg passed in by the factory wrapper.
local function make_middleware(profile, opts)
    opts = opts or {}
    local header_name = opts.report_only and
        "Content-Security-Policy-Report-Only" or
        "Content-Security-Policy"
    return function(req, res)
        local nonce = nonce_b64url(16)
        if not nonce then
            res:status(500)
            res:json({ error = "csp: nonce generation failed" })
            return 1
        end
        req.ctx = req.ctx or {}
        req.ctx.csp_nonce = nonce
        res:header(header_name, build_header(profile, nonce))
        return 0
    end
end

--- Pico-compatible CSP profile.
--
-- Allows inline `style="…"` attributes (Pico v2 classless uses them
-- on `<details>`, `<dialog>`, and a few form controls). `<script>`
-- and `<style>` blocks still require the per-request nonce.
--
-- @tparam ?{report_only=boolean} opts
-- @treturn function  Middleware suitable for `app.use("*", "/*", mw)`.
function csp.htmx(opts)
    return make_middleware("htmx", opts)
end

--- Strict CSP profile (no inline-style escape).
--
-- Blocks all inline styles, including `style="…"` attributes. Use
-- when you control every template and ship a CSS framework without
-- inline styles.
--
-- @tparam ?{report_only=boolean} opts
-- @treturn function
function csp.strict(opts)
    return make_middleware("strict", opts)
end

--- Generate a fresh CSP nonce (base64url, 128-bit, unpadded).
--
-- Exposed for callers that want to compose their own CSP middleware
-- but reuse Hull's nonce generation. Returns nil on `crypto.random`
-- failure.
--
-- @treturn string|nil
function csp.nonce()
    return nonce_b64url(16)
end

return csp
