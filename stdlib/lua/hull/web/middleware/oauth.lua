--- OAuth 2.0 / OIDC Authorization Code flow with PKCE.
--
-- @module hull.web.middleware.oauth
-- @license AGPL-3.0-or-later
--
-- Implements RFC 6749 (OAuth 2.0) + RFC 7636 (PKCE) + OIDC Core 1.0.
-- Designed for confidential or public OIDC clients that authenticate
-- users against a hosted IdP (Entra ID, Google, Auth0, Okta, etc.)
-- without rolling their own crypto + flow plumbing.
--
-- ## What this module does
--
-- Three routes per OIDC client app:
--   GET /auth/:provider/login    -> 302 to the IdP's authorize
--                                   endpoint with PKCE challenge +
--                                   signed state cookie.
--   GET /auth/:provider/callback -> exchanges the auth code for
--                                   tokens, verifies the ID token
--                                   signature against the IdP's JWKS
--                                   (x5c -> PEM), validates iss / aud
--                                   / exp / nonce, calls
--                                   find_user(provider, claims) ->
--                                   user, then on_login(req, res,
--                                   user, ctx) and 302s to the
--                                   return URL. ctx exposes the
--                                   provider name, raw claims, and
--                                   tokens for callers that need
--                                   them.
--   GET /auth/logout             -> clears state cookies; app's
--                                   on_logout (optional) clears its
--                                   own session.
--
-- ## What this module does NOT do
--
--   * Token refresh. The flow returns tokens; refresh_token (if
--     granted) is in the `tokens` table passed to on_login - the app
--     owns refresh strategy.
--   * Account linking. v1 is single-provider-per-user; linking the
--     same user across providers is a separately-reviewable feature.
--   * SAML, LDAP, enterprise SSO. OIDC-only.
--
-- ## Security
--
--   * State + nonce bound to the request via an HMAC-signed cookie
--     that the IdP echoes back; callback rejects on mismatch.
--     Defense against CSRF + cross-provider replay.
--   * PKCE (S256) protects against authorization-code interception
--     even when the redirect path is observable.
--   * ID token signature verified via the IdP's JWKS, fetched at
--     callback time and cached per process with kid-based refresh.
--   * `alg = "none"` rejected unconditionally; allowed-alg list
--     enforced before any key lookup (jwt.verify's gate).
--   * State cookie is HttpOnly + SameSite=Lax + 10-minute TTL.
--
-- ## Token handling (READ THIS BEFORE PERSISTING TOKENS)
--
--   * The 4th `ctx` arg passed to on_login contains
--     `{ provider, claims, tokens }`. `tokens` is the raw token
--     response from the IdP — including `access_token`,
--     `refresh_token`, and `id_token`. These are bearer
--     credentials: anyone with `_hull_sessions.data` read access
--     could call the IdP as the user.
--   * `session.login_handler`'s audit emission ALREADY scrubs
--     tokens + claims before writing to `_hull_audit_log` (see
--     round-3 hardening). The risk is the APPLICATION path.
--   * Apps that store tokens for later API calls MUST encrypt
--     them at rest. Recommended pattern: keep a 32-byte KEK in
--     env (or fs.read of a manifest-allowlisted file), use
--     `crypto.secretbox` to wrap each token, store the ciphertext
--     in the session row. Decrypt on demand at API-call time.
--   * Refresh tokens specifically should be considered long-
--     lived credentials. If you don't need offline API access,
--     drop them from the captured set inside your on_login
--     callback before persisting anything.
--
-- ## Usage
--
--     local oauth = require("hull.web.middleware.oauth")
--     oauth.init({
--         secret = env.get("OAUTH_STATE_SECRET"),  -- alias: state_secret
--         providers = {
--             entra = {
--                 preset      = "microsoft",
--                 tenant      = "00000000-0000-0000-0000-000000000000",
--                 client_id   = env.get("ENTRA_CLIENT_ID"),
--                 client_secret = env.get("ENTRA_CLIENT_SECRET"),
--                 scopes      = { "openid", "profile", "email" },
--             },
--             google = {
--                 preset      = "google",
--                 client_id   = env.get("GOOGLE_CLIENT_ID"),
--                 client_secret = env.get("GOOGLE_CLIENT_SECRET"),
--             },
--         },
--         find_user = function(provider, claims)
--             return find_or_create_user(claims.sub, claims.email)
--         end,
--         -- on_login matches hull/web/auth-flows shape so a single
--         -- session.login_handler(cookie) wires both.
--         on_login = session.login_handler(cookie),
--         on_logout = session.logout_handler(cookie),
--     })
--     oauth.routes(app)

local crypto      = require("hull.crypto")
local envelope    = require("hull.crypto.envelope")
local cookie      = require("hull.web.cookie")
local json        = require("hull.json")
local jwt         = require("hull.jwt")
local time        = require("hull.time")
local http_client = require("hull.http-client")
local log         = require("hull.log")

local oauth = {}

-- ── Module state ───────────────────────────────────────────────────

local _state = {
    state_secret_hex = nil,  -- secret bytes pre-encoded as hex (HMAC takes hex)
    -- redirect_uri origin resolution (see compute_redirect_uri):
    --   base_url set        -> use it verbatim (recommended).
    --   trust_proxy = true  -> honor X-Forwarded-Proto/Host (trusted proxy).
    --   otherwise (default) -> Host header + https; ignore forwarded headers so
    --                          a directly-exposed app can't have the redirect_uri
    --                          sent to the IdP poisoned via a spoofed header.
    base_url         = nil,
    trust_proxy      = false,
    state_cookie     = "_oauth_state",
    -- Cookie path scoping. Defaults to "/auth" so the state cookie
    -- isn't sent on every request (only those matching the auth
    -- routes). Apps that mount login/callback at a non-/auth prefix
    -- should override to the common ancestor of their custom paths.
    state_cookie_path = "/auth",
    -- Cookie SameSite policy. Default "Lax" works for the standard
    -- response_type=code flow (IdP redirects back via top-level
    -- GET, Lax permits). OIDC flows using response_mode=form_post
    -- (Azure AD by default for hybrid flows, some enterprise IdPs)
    -- have the IdP POST back; Lax blocks POSTs from cross-origin,
    -- so the state cookie won't reach the callback. Override to
    -- "None" + require Secure=true to support those flows.
    state_cookie_samesite = "Lax",
    state_ttl        = 600,
    providers        = {},
    -- find_user(provider, claims) -> user-object — required when
    -- on_login is wired. Lets on_login use the same (req, res,
    -- user) signature as hull/web/auth-flows so a single login
    -- handler (typically session.login_handler(cookie)) works for
    -- both auth sources. The provider + claims + tokens are still
    -- available via the optional 4th `ctx` arg on on_login.
    find_user        = nil,
    on_login         = nil,
    on_logout        = nil,
    -- Route templates - `{provider}` is replaced with `:provider` at
    -- mount time so app.get sees Hull's native path-param syntax.
    login_path       = "/auth/{provider}/login",
    callback_path    = "/auth/{provider}/callback",
    logout_path      = "/auth/logout",
    -- JWKS cache: provider_name -> { fetched_at, by_kid = { kid -> pem } }
    _jwks_cache      = {},
    -- TTL on cached JWKS in seconds (forces a refresh past this age
    -- even if the kid is still present). Default 1 hour. IdPs may
    -- rotate a key while reusing the same kid for emergency
    -- revocation; without a TTL the stale PEM would verify forever.
    jwks_ttl         = 3600,
}

-- ── Provider presets ───────────────────────────────────────────────

local PRESETS = {
    google = function(_opts)
        return {
            authorization_endpoint = "https://accounts.google.com/o/oauth2/v2/auth",
            token_endpoint         = "https://oauth2.googleapis.com/token",
            jwks_uri               = "https://www.googleapis.com/oauth2/v3/certs",
            issuer                 = "https://accounts.google.com",
        }
    end,
    microsoft = function(opts)
        -- Tenant default `common` accepts any Microsoft account
        -- (consumer or work/school). For a specific Entra tenant pass
        -- its GUID or domain. `consumers` = personal only,
        -- `organizations` = work/school only.
        --
        -- Round-10 HIGH-3: When using /common the ID token's `iss`
        -- claim is the tenant id (not "common") — strict iss
        -- equality would 100% reject. Multi-tenant presets now emit
        -- an `issuer_pattern` (Lua: gsub-friendly regex) which
        -- handle_callback checks as a fallback when strict `issuer`
        -- equality misses. Single-tenant configs (tenant = "<guid>")
        -- still get strict equality and nothing else.
        local tenant = opts.tenant or "common"
        local base = "https://login.microsoftonline.com/" .. tenant
        local cfg = {
            authorization_endpoint = base .. "/oauth2/v2.0/authorize",
            token_endpoint         = base .. "/oauth2/v2.0/token",
            jwks_uri               = base .. "/discovery/v2.0/keys",
            issuer                 = base .. "/v2.0",
        }
        if tenant == "common" or tenant == "organizations"
           or tenant == "consumers" then
            -- Lua pattern: matches /{guid or domain}/v2.0 — Microsoft
            -- emits either a 36-char tenant GUID OR a verified domain.
            cfg.issuer_pattern =
                "^https://login%.microsoftonline%.com/[%w%-%.]+/v2%.0$"
        end
        return cfg
    end,
}

-- ── Helpers ────────────────────────────────────────────────────────

-- Cap layer's hmac_sha256 takes the key as a hex string. We pre-
-- encode the secret bytes once at init and reuse the hex form.
-- Thin aliases over crypto.hex_encode / crypto.hex_decode so
-- the implementation lives in one place (cap/crypto.c).
local function bytes_to_hex(s) return crypto.hex_encode(s) end
local function hex_to_bytes(h) return crypto.hex_decode(h) end

-- Cryptographically-random URL-safe string (base64url-encoded).
local function random_urlsafe(n_bytes)
    return crypto.base64url_encode(crypto.random(n_bytes))
end

-- PKCE per RFC 7636: verifier is 32 random bytes (~43 base64url chars);
-- challenge = base64url(SHA-256(verifier)).
local function pkce_pair()
    local verifier = random_urlsafe(32)
    local sha_hex = crypto.sha256(verifier)
    local challenge = crypto.base64url_encode(hex_to_bytes(sha_hex))
    return verifier, challenge
end

-- Same-origin guard for the return_to query parameter. Without this,
-- a request like `/auth/google/login?return_to=https://evil.com`
-- signs evil.com into the state cookie; the callback then redirects
-- the just-authenticated user there. Classic open-redirect that
-- weaponizes the host's own login page for phishing.
--
-- Accept only same-origin paths starting with "/" followed by NOT
-- "/" and NOT "\". Reject scheme-relative ("//evil.com"), backslash-
-- escapes that some clients normalize as paths ("/\evil.com"),
-- absolute URLs ("http://", "https://"), header-injection bytes,
-- and absurdly-long values. Fall back to "/" on any rejection.
local function safe_return_to(s)
    if type(s) ~= "string" or s == "" then return "/" end
    if #s > 200 then return "/" end
    if s:find("[\r\n%z]") then return "/" end
    -- First char must be "/", second char (if any) must not be "/" or "\".
    if s:sub(1, 1) ~= "/" then return "/" end
    local c2 = s:sub(2, 2)
    if c2 == "/" or c2 == "\\" then return "/" end
    return s
end

-- URL-encode a value (RFC 3986 unreserved set untouched).
local function urlenc(s)
    return (s:gsub("([^A-Za-z0-9%-._~])", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

-- Build a URL with sorted query params (deterministic for tests).
local function build_url(base, params)
    local parts = {}
    for k, v in pairs(params) do
        parts[#parts + 1] = urlenc(k) .. "=" .. urlenc(v)
    end
    table.sort(parts)
    if #parts == 0 then return base end
    local sep = base:find("?", 1, true) and "&" or "?"
    return base .. sep .. table.concat(parts, "&")
end

-- Standard base64 (JWKS x5c) -> base64url, so crypto.base64url_decode
-- can consume it. Strips whitespace + padding, converts +/=- mapping.
local function b64_to_b64url(s)
    s = s:gsub("[%s\n\r]", "")
    s = s:gsub("=", "")
    s = s:gsub("+", "-")
    s = s:gsub("/", "_")
    return s
end

-- ── State cookie: sign + verify ────────────────────────────────────
--
-- Payload: { provider, verifier, state, nonce, return_to, exp }.
-- Signature framing (base64url(JSON) || "." || hex(HMAC-SHA256))
-- lives in hull.crypto.envelope; the expiry check is OAuth-
-- specific and stays here.

local function sign_state(payload)
    payload.exp = time.now() + _state.state_ttl
    return envelope.sign(payload, _state.state_secret_hex)
end

local function verify_state(cookie_value)
    if not cookie_value or cookie_value == "" then return nil, "empty" end
    local env, err = envelope.verify(cookie_value, _state.state_secret_hex)
    if not env then return nil, err end
    if type(env.exp) ~= "number" or time.now() >= env.exp then
        return nil, "expired"
    end
    return env
end

-- ── JWKS cache ─────────────────────────────────────────────────────
--
-- JWKS endpoints publish a JSON object `{keys: [{kty, kid, x5c, alg,
-- use, ...}, ...]}`. We fetch lazily, cache by provider, and rebuild
-- on a kid miss (handles IdP key rotation). x5c[0] is a base64-DER
-- X.509 cert; crypto.x509_pubkey_pem extracts the SPKI as PEM so
-- jwt.verify (via crypto.verify) can consume it.

local function refresh_jwks(provider_name)
    local cfg = _state.providers[provider_name]
    if not cfg then return nil, "unknown provider" end
    local resp = http_client.async.get(cfg.jwks_uri)
    if not resp or resp.status ~= 200 or not resp.body then
        return nil, "jwks fetch failed: " .. tostring(resp and resp.status)
    end
    local doc = json.decode(resp.body)
    if type(doc) ~= "table" or type(doc.keys) ~= "table" then
        return nil, "jwks: malformed response"
    end
    local by_kid = {}
    for _, k in ipairs(doc.keys) do
        if type(k) == "table" and k.kid and type(k.x5c) == "table"
           and type(k.x5c[1]) == "string" then
            local der = crypto.base64url_decode(b64_to_b64url(k.x5c[1]))
            if der then
                local pem = crypto.x509_pubkey_pem(der)
                if pem then by_kid[k.kid] = pem end
            end
        end
    end
    _state._jwks_cache[provider_name] = {
        fetched_at = time.now(),
        by_kid = by_kid,
    }
    return by_kid
end

local function jwks_resolver(provider_name)
    return function(kid, _alg)
        local cache = _state._jwks_cache[provider_name]
        local ttl   = _state.jwks_ttl or 3600
        local fresh = cache
                      and (time.now() - (cache.fetched_at or 0)) < ttl
        if fresh and cache.by_kid[kid] then
            return cache.by_kid[kid]
        end
        -- Stale OR unknown kid: refresh once. A still-unknown kid
        -- after refresh returns nil; signature verify then fails.
        local by_kid = refresh_jwks(provider_name)
        if by_kid then return by_kid[kid] end
        return nil
    end
end

-- ── Route handlers ─────────────────────────────────────────────────

local function compute_redirect_uri(req, provider_name)
    local path = _state.callback_path:gsub("{provider}", provider_name)
    if _state.base_url then return _state.base_url .. path end
    local proto, host
    if _state.trust_proxy then
        proto = req.headers["x-forwarded-proto"] or "https"
        -- X-Forwarded-Host can be a chain ("a.com, lb.internal"); take the
        -- client-facing (first) hop.
        local xfh = req.headers["x-forwarded-host"]
        local first = xfh and xfh:match("^%s*([^,]+)")
        host = (first and first:match("^%s*(.-)%s*$")) or req.headers.host or "localhost"
    else
        -- Naked case: no base_url, no trusted proxy. The Host header is
        -- client-controllable, so the redirect_uri host is only as trustworthy
        -- as the deployment. Default proto http (dev-friendly); production
        -- should set base_url. The init warning surfaces this.
        proto = "http"
        host  = req.headers.host or "localhost"
    end
    return proto .. "://" .. host .. path
end

-- GET /auth/:provider/login
local function handle_login(req, res)
    local provider_name = req.params and req.params.provider
    local cfg = provider_name and _state.providers[provider_name]
    if not cfg then return res:status(404):html("unknown provider") end

    local verifier, challenge = pkce_pair()
    local state_value = random_urlsafe(16)
    local nonce_value = random_urlsafe(16)
    local return_to = safe_return_to(req.query and req.query.return_to)

    -- Bind the envelope to this provider so a cookie minted for
    -- /auth/microsoft/login can't be replayed against /auth/google/callback.
    local signed = sign_state({
        provider  = provider_name,
        verifier  = verifier,
        state     = state_value,
        nonce     = nonce_value,
        return_to = return_to,
    })
    res:header("Set-Cookie", cookie.serialize(
        _state.state_cookie, signed,
        { httponly = true,
          samesite = _state.state_cookie_samesite,
          -- SameSite=None mandates Secure per spec, and browsers
          -- silently drop None-without-Secure. Set both together.
          secure   = (_state.state_cookie_samesite == "None"),
          path     = _state.state_cookie_path,
          max_age  = _state.state_ttl }))

    local params = {
        client_id     = cfg.client_id,
        redirect_uri  = compute_redirect_uri(req, provider_name),
        response_type = "code",
        scope         = table.concat(cfg.scopes, " "),
        state         = state_value,
        nonce         = nonce_value,
        code_challenge        = challenge,
        code_challenge_method = "S256",
    }
    res:redirect(build_url(cfg.authorization_endpoint, params))
end

-- GET /auth/:provider/callback?code=...&state=...
local function handle_callback(req, res)
    local provider_name = req.params and req.params.provider
    local cfg = provider_name and _state.providers[provider_name]
    if not cfg then return res:status(404):html("unknown provider") end

    -- Clear the state cookie unconditionally on every callback,
    -- success or failure. State cookies are single-use by design;
    -- a failed callback (state verify failure, token exchange
    -- 502, id_token verify failure, etc.) used to leave the
    -- cookie alive for state_ttl (600s default), giving an
    -- attacker a window to spam failure paths and pin the
    -- victim's cookie at a known value. Cleared here covers
    -- every branch below.
    res:header("Set-Cookie",
        cookie.clear(_state.state_cookie,
                     { path = _state.state_cookie_path }))

    -- 1. Read + verify state cookie.
    local cookies = cookie.parse(req.headers.cookie or "")
    local env, err = verify_state(cookies[_state.state_cookie])
    if not env then
        log.warn("oauth: state verify failed: " .. tostring(err))
        return res:status(400):html("auth failed")
    end
    if env.provider ~= provider_name then
        return res:status(400):html("auth failed")
    end

    -- 2. Match query state against envelope.
    local q = req.query or {}
    if q.error then
        log.warn("oauth: provider returned error: " .. tostring(q.error))
        return res:status(400):html("auth failed")
    end
    if not q.code or q.state ~= env.state then
        return res:status(400):html("auth failed")
    end

    -- 3. Token exchange. redirect_uri MUST match the one sent at
    --    /login per OAuth 2.0; we compute it the same way both times.
    local redirect_uri = compute_redirect_uri(req, provider_name)
    local body_params = {
        grant_type    = "authorization_code",
        code          = q.code,
        redirect_uri  = redirect_uri,
        client_id     = cfg.client_id,
        code_verifier = env.verifier,
    }
    if cfg.client_secret then
        body_params.client_secret = cfg.client_secret
    end
    local body_parts = {}
    for k, v in pairs(body_params) do
        body_parts[#body_parts + 1] = urlenc(k) .. "=" .. urlenc(v)
    end
    local resp = http_client.async.post(cfg.token_endpoint,
        table.concat(body_parts, "&"),
        { headers = {
            ["Content-Type"] = "application/x-www-form-urlencoded",
            ["Accept"] = "application/json",
        }})
    if not resp or resp.status ~= 200 or not resp.body then
        log.warn("oauth: token exchange failed: " ..
                 tostring(resp and resp.status))
        return res:status(502):html("auth failed")
    end
    local tokens = json.decode(resp.body)
    if type(tokens) ~= "table" or type(tokens.id_token) ~= "string" then
        return res:status(502):html("auth failed")
    end

    -- 4. Verify ID token. The allowlist is the union of asymmetric
    --    algs supported by major IdPs; HS256 is excluded because OIDC
    --    providers don't sign ID tokens with HMAC.
    local claims, jerr = jwt.verify(tokens.id_token,
        jwks_resolver(provider_name),
        { algs = { "RS256", "RS384", "RS512", "PS256",
                   "ES256", "ES384" }})
    if not claims then
        log.warn("oauth: id_token verify failed: " .. tostring(jerr))
        return res:status(400):html("auth failed")
    end

    -- 5. Required OIDC claim checks beyond signature + exp.
    -- Round-10 HIGH-3: when the preset is multi-tenant (Microsoft
    -- `common` / `organizations` / `consumers`), cfg.issuer_pattern
    -- accepts any matching per-tenant issuer.
    local iss_ok = (claims.iss == cfg.issuer)
    if not iss_ok and type(cfg.issuer_pattern) == "string"
       and type(claims.iss) == "string" then
        iss_ok = claims.iss:match(cfg.issuer_pattern) ~= nil
    end
    if not iss_ok then
        log.warn("oauth: iss mismatch: " .. tostring(claims.iss))
        return res:status(400):html("auth failed")
    end
    local aud_ok = false
    if type(claims.aud) == "string" then
        aud_ok = (claims.aud == cfg.client_id)
    elseif type(claims.aud) == "table" then
        for _, a in ipairs(claims.aud) do
            if a == cfg.client_id then aud_ok = true; break end
        end
    end
    if not aud_ok then return res:status(400):html("auth failed") end
    if claims.nonce ~= env.nonce then
        return res:status(400):html("auth failed")
    end

    -- (State cookie was cleared at the top of the handler — every
    -- callback consumes the cookie regardless of outcome.)

    -- 7. Resolve claims -> app's user object via find_user, then
    --    hand off via on_login(req, res, user, ctx). on_login's
    --    signature now matches hull/web/auth-flows so a single
    --    session.login_handler(cookie) can be wired for both
    --    auth sources. The OIDC-specific context (provider name,
    --    raw claims, tokens) is still available via the 4th arg
    --    for apps that need it. Return value of on_login (if a
    --    string) overrides the post-login redirect target.
    local target = env.return_to or "/"
    if _state.on_login then
        local user = _state.find_user(provider_name, claims)
        if not user then
            return res:status(400):html("auth failed: user resolution returned nil")
        end
        local ctx = {
            provider = provider_name,
            claims   = claims,
            tokens   = tokens,
        }
        local v = _state.on_login(req, res, user, ctx)
        if type(v) == "string" then target = v end
    end
    res:redirect(target)
end

-- GET /auth/logout
local function handle_logout(req, res)
    res:header("Set-Cookie",
        cookie.clear(_state.state_cookie,
                     { path = _state.state_cookie_path }))
    local target = "/"
    if _state.on_logout then
        local v = _state.on_logout(req, res)
        if type(v) == "string" then target = v end
    end
    res:redirect(target)
end

-- ── Public API ─────────────────────────────────────────────────────

--- Initialize the module. Must be called once at app startup.
function oauth.init(opts)
    if type(opts) ~= "table" then
        error("oauth.init: opts table required")
    end
    -- Canonical `secret`; back-compat alias `state_secret` (same HMAC key).
    local secret = opts.secret or opts.state_secret
    if type(secret) ~= "string" or #secret < 32 then
        error("oauth.init: secret must be a string >= 32 bytes "
              .. "(same HMAC primitive as hull/web/auth-flows; pick "
              .. "one floor)")
    end
    _state.state_secret_hex = bytes_to_hex(secret)
    -- redirect_uri origin (see compute_redirect_uri): explicit base_url wins,
    -- else trust_proxy gates the spoofable X-Forwarded-Proto/Host headers.
    if opts.base_url ~= nil then
        _state.base_url = tostring(opts.base_url):gsub("/+$", "")
    end
    _state.trust_proxy = opts.trust_proxy == true
    if not _state.base_url and not _state.trust_proxy then
        log.warn("oauth: neither base_url nor trust_proxy set; the "
              .. "redirect_uri sent to the IdP is built from the client-"
              .. "controllable Host header. Set base_url = "
              .. "\"https://app.example.com\" in production (or trust_proxy = "
              .. "true behind a proxy that sets X-Forwarded-Host).")
    end
    _state.state_cookie = opts.state_cookie or _state.state_cookie
    _state.state_cookie_path =
        opts.state_cookie_path or _state.state_cookie_path
    if opts.state_cookie_samesite ~= nil then
        local s = opts.state_cookie_samesite
        if s ~= "Lax" and s ~= "Strict" and s ~= "None" then
            error("oauth.init: state_cookie_samesite must be "
                  .. "'Lax', 'Strict', or 'None'")
        end
        _state.state_cookie_samesite = s
    end
    _state.state_ttl    = opts.state_ttl or _state.state_ttl
    _state.jwks_ttl     = opts.jwks_ttl  or _state.jwks_ttl
    if opts.on_login ~= nil and type(opts.find_user) ~= "function" then
        error("oauth.init: find_user(provider, claims) -> user is "
              .. "required when on_login is set. See module docs.")
    end
    _state.find_user    = opts.find_user
    _state.on_login     = opts.on_login
    _state.on_logout    = opts.on_logout

    if type(opts.providers) ~= "table" then
        error("oauth.init: providers table required")
    end
    _state.providers = {}
    for name, p in pairs(opts.providers) do
        if type(p) ~= "table" then
            error("oauth.init: provider '" .. name .. "' must be a table")
        end
        local resolved = {}
        if p.preset then
            local fn = PRESETS[p.preset]
            if not fn then
                error("oauth.init: unknown preset '" .. tostring(p.preset)
                      .. "' (known: google, microsoft)")
            end
            for k, v in pairs(fn(p)) do resolved[k] = v end
        end
        -- Explicit values override the preset.
        for _, k in ipairs({"authorization_endpoint", "token_endpoint",
                            "jwks_uri", "issuer"}) do
            if p[k] then resolved[k] = p[k] end
        end
        for _, k in ipairs({"authorization_endpoint", "token_endpoint",
                            "jwks_uri", "issuer"}) do
            if type(resolved[k]) ~= "string" then
                error("oauth.init: provider '" .. name ..
                      "' missing " .. k ..
                      " (use a preset or supply explicitly)")
            end
        end
        if type(p.client_id) ~= "string" then
            error("oauth.init: provider '" .. name .. "' missing client_id")
        end
        resolved.client_id     = p.client_id
        resolved.client_secret = p.client_secret  -- optional (PKCE-only)
        resolved.scopes        = p.scopes or { "openid", "profile", "email" }
        if type(resolved.scopes) ~= "table" then
            error("oauth.init: provider '" .. name ..
                  "' scopes must be a table")
        end
        _state.providers[name] = resolved
    end
end

--- Mount the three OIDC routes on `app`. Call after oauth.init().
function oauth.routes(app)
    if not _state.state_secret_hex then
        error("oauth.routes: oauth.init() must be called first")
    end
    app.get(_state.login_path:gsub("{provider}", ":provider"),
            handle_login)
    app.get(_state.callback_path:gsub("{provider}", ":provider"),
            handle_callback)
    app.get(_state.logout_path, handle_logout)
end

-- Test helpers (not part of the public contract). Lets tests round-
-- trip sign/verify without going through the full HTTP flow.
oauth._test = {
    pkce_pair       = pkce_pair,
    sign_state      = sign_state,
    verify_state    = verify_state,
    b64_to_b64url   = b64_to_b64url,
    refresh_jwks    = refresh_jwks,
    safe_return_to  = safe_return_to,
    reset = function()
        _state.state_secret_hex = nil
        _state.providers        = {}
        _state.find_user        = nil
        _state.on_login         = nil
        _state.on_logout        = nil
        _state._jwks_cache      = {}
    end,
}

return oauth
