/**
 * @file hull:web:middleware:oauth
 * @module hull:web:middleware:oauth
 * @description OAuth 2.0 / OIDC Authorization Code flow with PKCE.
 *
 * Lua parity: `hull.web.middleware.oauth` (snake_case keys ↔ camelCase
 * here). Same three-route surface, same PKCE/state/JWKS semantics.
 *
 * @license AGPL-3.0-or-later
 *
 * ## What this module does
 *
 *   GET /auth/:provider/login    -> 302 to IdP authorize endpoint with
 *                                   PKCE challenge + signed state cookie.
 *   GET /auth/:provider/callback -> exchanges code for tokens, verifies
 *                                   ID token via JWKS x5c, validates
 *                                   iss / aud / exp / nonce, calls
 *                                   onLogin(req, res, provider, claims,
 *                                   tokens), 302s to return URL.
 *   GET /auth/logout             -> clears state cookies; optional
 *                                   onLogout clears app session.
 *
 * ## Security
 *
 *   - State + nonce HMAC-signed cookie; the IdP echoes them back, the
 *     callback rejects on mismatch (CSRF + cross-provider replay).
 *   - PKCE S256 protects against auth-code interception.
 *   - ID token verified via JWKS (cached per process, kid-based
 *     refresh). x5c base64-DER -> SPKI PEM via crypto.x509PubkeyPem.
 *   - `alg = "none"` rejected unconditionally; allowed-alg list
 *     enforced before any key lookup (jwt.verify's gate).
 *   - State cookie is HttpOnly + SameSite=Lax + 10-minute TTL.
 *
 * ## Token handling (READ THIS BEFORE PERSISTING TOKENS)
 *
 *   - The 4th `ctx` arg passed to onLogin contains
 *     { provider, claims, tokens }. `tokens` is the raw token
 *     response from the IdP — including access_token,
 *     refresh_token, and id_token. These are bearer credentials:
 *     anyone with _hull_sessions.data read access could call the
 *     IdP as the user.
 *   - session.loginHandler's audit emission ALREADY scrubs
 *     tokens + claims before writing to _hull_audit_log (round-3
 *     hardening). The risk is the APPLICATION path.
 *   - Apps that store tokens for later API calls MUST encrypt them
 *     at rest. Recommended pattern: keep a 32-byte KEK in env (or
 *     fs.read of a manifest-allowlisted file), use crypto.secretbox
 *     to wrap each token, store ciphertext in the session row.
 *     Decrypt on demand at API-call time.
 *   - Refresh tokens specifically should be considered long-lived
 *     credentials. If you don't need offline API access, drop them
 *     from the captured set inside your onLogin before persisting.
 *
 * @example
 *   import { oauth } from "hull:web:middleware:oauth";
 *   oauth.init({
 *       stateSecret: env.get("OAUTH_STATE_SECRET"),
 *       providers: {
 *           entra: {
 *               preset: "microsoft",
 *               tenant: "00000000-0000-0000-0000-000000000000",
 *               clientId: env.get("ENTRA_CLIENT_ID"),
 *               clientSecret: env.get("ENTRA_CLIENT_SECRET"),
 *               scopes: ["openid", "profile", "email"],
 *           },
 *           google: {
 *               preset: "google",
 *               clientId: env.get("GOOGLE_CLIENT_ID"),
 *               clientSecret: env.get("GOOGLE_CLIENT_SECRET"),
 *           },
 *       },
 *       onLogin: async (req, res, provider, claims, tokens) => {
 *           const user = await findOrCreateUser(claims.sub, claims.email);
 *           session.createForUser(req, res, user.id);
 *           return "/";
 *       },
 *   });
 *   oauth.routes(app);
 */

import { crypto }     from "hull:crypto";
import { envelope }   from "hull:crypto:envelope";
import { cookie }     from "hull:web:cookie";
import { json }       from "hull:json";
import { jwt }        from "hull:jwt";
import { time }       from "hull:time";
import { httpClient } from "hull:http-client";
import { log }        from "hull:log";

// ── Module state ───────────────────────────────────────────────────

const _state = {
    stateSecretHex: null,
    stateCookie:    "_oauth_state",
    // Cookie path scoping. Defaults to "/auth" so the state cookie
    // isn't sent on every request (only those matching the auth
    // routes). Apps that mount login/callback at a non-/auth prefix
    // should override to the common ancestor of their custom paths.
    stateCookiePath: "/auth",
    // Cookie SameSite policy. Default "Lax" works for response_type=
    // code (top-level GET redirect). OIDC flows using response_mode=
    // form_post (Azure AD by default for hybrid flows, some
    // enterprise IdPs) POST back from the IdP; Lax blocks the cookie
    // on cross-origin POST. Override to "None" + require Secure=true
    // to support those flows.
    stateCookieSameSite: "Lax",
    stateTtl:       600,
    providers:      {},  // name -> resolved cfg
    // findUser(provider, claims) -> user-object — required when
    // onLogin is set. Lets onLogin use the same (req, res, user)
    // signature as hull/web/auth-flows so a single login handler
    // (typically session.loginHandler(cookie)) works for both
    // auth sources. provider/claims/tokens are still available
    // via the 4th `ctx` arg on onLogin.
    findUser:       null,
    onLogin:        null,
    onLogout:       null,
    loginPath:      "/auth/{provider}/login",
    callbackPath:   "/auth/{provider}/callback",
    logoutPath:     "/auth/logout",
    _jwksCache:     {},  // name -> { fetchedAt, byKid: { kid: pem } }
};

// ── Provider presets ───────────────────────────────────────────────

const PRESETS = {
    google: (_opts) => ({
        authorizationEndpoint: "https://accounts.google.com/o/oauth2/v2/auth",
        tokenEndpoint:         "https://oauth2.googleapis.com/token",
        jwksUri:               "https://www.googleapis.com/oauth2/v3/certs",
        issuer:                "https://accounts.google.com",
    }),
    microsoft: (opts) => {
        // Tenant default `common` accepts any Microsoft account.
        // For a specific Entra tenant pass GUID or domain.
        // Round-10 HIGH-3: multi-tenant presets emit issuerPattern
        // so handleCallback's iss check falls back to a pattern
        // match. Without it, strict iss equality 100% rejects
        // because Microsoft's id_token returns /{tenant-guid}/
        // (not /common/). See Lua sibling.
        const tenant = opts.tenant || "common";
        const base = "https://login.microsoftonline.com/" + tenant;
        const cfg = {
            authorizationEndpoint: base + "/oauth2/v2.0/authorize",
            tokenEndpoint:         base + "/oauth2/v2.0/token",
            jwksUri:               base + "/discovery/v2.0/keys",
            issuer:                base + "/v2.0",
        };
        if (tenant === "common" || tenant === "organizations"
            || tenant === "consumers") {
            cfg.issuerPattern =
                /^https:\/\/login\.microsoftonline\.com\/[\w\-\.]+\/v2\.0$/;
        }
        return cfg;
    },
};

// ── Helpers ────────────────────────────────────────────────────────

// crypto.hmacSha256 takes the key as a hex string. We pre-encode the
// secret bytes once at init and reuse the hex form. Kept local
// (not aliased to crypto.hexEncode) because JS strings with code
// points >= 0x80 UTF-8-inflate when crossing the C boundary via
// JS_ToCStringLen; binary-string inputs (state secrets with high
// bytes, ID-token hashes) must round-trip byte-identically.
function bytesToHex(s) {
    let h = "";
    for (let i = 0; i < s.length; i++) {
        // & 0xff matches the sibling helpers in totp.js + auth-flows.js;
        // without it, a state_secret with code points > 0xff (the
        // case the comment block above claims to handle) emits 3+
        // hex chars per char and the round-trip silently breaks.
        const c = s.charCodeAt(i) & 0xff;
        h += (c < 16 ? "0" : "") + c.toString(16);
    }
    return h;
}

function hexToBytes(h) {
    const u8 = new Uint8Array(h.length >> 1);
    for (let i = 0, j = 0; i < h.length; i += 2, j++) {
        u8[j] = parseInt(h.substr(i, 2), 16);
    }
    return u8;
}

function randomUrlsafe(nBytes) {
    return crypto.base64urlEncode(crypto.random(nBytes));
}

// PKCE per RFC 7636: verifier is 32 random bytes (~43 base64url chars),
// challenge = base64url(SHA-256(verifier)).
function pkcePair() {
    const verifier = randomUrlsafe(32);
    const shaHex = crypto.sha256(verifier);
    const challenge = crypto.base64urlEncode(hexToBytes(shaHex).buffer);
    return [verifier, challenge];
}

// Same-origin guard for the return_to query parameter. Without this,
// a request like `/auth/google/login?return_to=https://evil.com`
// signs evil.com into the state cookie; the callback then redirects
// the just-authenticated user there. Classic open-redirect that
// weaponizes the host's own login page for phishing.
//
// Accept only same-origin paths starting with "/" followed by NOT
// "/" and NOT "\". Reject scheme-relative ("//evil.com"), backslash-
// escapes that some clients normalize as paths ("/\evil.com"),
// absolute URLs ("http://", "https://"), header-injection bytes,
// and absurdly-long values. Fall back to "/" on any rejection.
function safeReturnTo(s) {
    if (typeof s !== "string" || s === "") return "/";
    if (s.length > 200) return "/";
    if (/[\r\n\0]/.test(s)) return "/";
    if (s.charAt(0) !== "/") return "/";
    const c2 = s.charAt(1);
    if (c2 === "/" || c2 === "\\") return "/";
    return s;
}

function urlenc(s) {
    return encodeURIComponent(s).replace(/[!'()*]/g, (c) =>
        "%" + c.charCodeAt(0).toString(16).toUpperCase());
}

// Sorted query params for deterministic test output.
function buildUrl(base, params) {
    const parts = [];
    for (const k in params) {
        if (Object.prototype.hasOwnProperty.call(params, k)) {
            parts.push(urlenc(k) + "=" + urlenc(params[k]));
        }
    }
    parts.sort();
    if (parts.length === 0) return base;
    const sep = base.indexOf("?") >= 0 ? "&" : "?";
    return base + sep + parts.join("&");
}

// Standard base64 (JWKS x5c) -> base64url for direct binary decoding.
function b64ToB64url(s) {
    return s.replace(/[\s\r\n]/g, "")
            .replace(/=/g, "")
            .replace(/\+/g, "-")
            .replace(/\//g, "_");
}

// QuickJS strings corrupt binary > 127 through UTF-8 inflation. JWKS
// x5c is binary DER, so we use a Uint8Array decoder (mirrors jwt.js's
// approach for binary JWS signatures).
const B64_DEC = (() => {
    const t = new Int8Array(256).fill(-1);
    const a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (let i = 0; i < a.length; i++) t[a.charCodeAt(i)] = i;
    t["+".charCodeAt(0)] = 62;
    t["/".charCodeAt(0)] = 63;
    return t;
})();
function b64Lookup(s, i) {
    const c = s.charCodeAt(i);
    if (c > 255) return -1;
    return B64_DEC[c];
}
function base64urlToBytes(s) {
    let n = s.length;
    while (n > 0 && s.charCodeAt(n - 1) === 61) n--;
    const groups = n >> 2;
    const tail = n & 3;
    if (tail === 1) return null;
    let outLen = groups * 3;
    if (tail === 2) outLen += 1;
    else if (tail === 3) outLen += 2;
    const u8 = new Uint8Array(outLen);
    let si = 0, di = 0;
    for (let g = 0; g < groups; g++) {
        const a = b64Lookup(s, si++);
        const b = b64Lookup(s, si++);
        const c = b64Lookup(s, si++);
        const d = b64Lookup(s, si++);
        if ((a | b | c | d) < 0) return null;
        u8[di++] = (a << 2) | (b >> 4);
        u8[di++] = ((b & 0xf) << 4) | (c >> 2);
        u8[di++] = ((c & 0x3) << 6) | d;
    }
    if (tail === 2) {
        const a = b64Lookup(s, si++);
        const b = b64Lookup(s, si++);
        if ((a | b) < 0) return null;
        u8[di++] = (a << 2) | (b >> 4);
    } else if (tail === 3) {
        const a = b64Lookup(s, si++);
        const b = b64Lookup(s, si++);
        const c = b64Lookup(s, si++);
        if ((a | b | c) < 0) return null;
        u8[di++] = (a << 2) | (b >> 4);
        u8[di++] = ((b & 0xf) << 4) | (c >> 2);
    }
    return u8;
}

// ── State cookie ───────────────────────────────────────────────────
//
// Payload: { provider, verifier, state, nonce, return_to, exp }.
// Signature framing comes from hull:crypto:envelope; the expiry
// check is OAuth-specific and stays here.

function signState(payload) {
    payload.exp = time.now() + _state.stateTtl;
    return envelope.sign(payload, _state.stateSecretHex);
}

function verifyState(cookieValue) {
    if (!cookieValue) return [null, "empty"];
    const r = envelope.verify(cookieValue, _state.stateSecretHex);
    if (!r[0]) return [null, r[1]];
    const env = r[0];
    if (typeof env.exp !== "number" || time.now() >= env.exp) {
        return [null, "expired"];
    }
    return [env, null];
}

// ── JWKS cache ─────────────────────────────────────────────────────

async function refreshJwks(providerName) {
    const cfg = _state.providers[providerName];
    if (!cfg) return null;
    const resp = await httpClient.async.get(cfg.jwksUri);
    if (!resp || resp.status !== 200 || !resp.body) return null;
    let doc;
    try { doc = json.decode(resp.body); }
    catch (_e) { return null; }
    if (!doc || !Array.isArray(doc.keys)) return null;
    const byKid = {};
    for (const k of doc.keys) {
        if (k && k.kid && Array.isArray(k.x5c) && typeof k.x5c[0] === "string") {
            const u8 = base64urlToBytes(b64ToB64url(k.x5c[0]));
            if (u8) {
                const pem = crypto.x509PubkeyPem(u8.buffer);
                if (pem) byKid[k.kid] = pem;
            }
        }
    }
    _state._jwksCache[providerName] = { fetchedAt: time.now(), byKid };
    return byKid;
}

function jwksResolver(providerName) {
    return async (kid, _alg) => {
        const cache = _state._jwksCache[providerName];
        if (cache && cache.byKid[kid]) return cache.byKid[kid];
        const byKid = await refreshJwks(providerName);
        return byKid ? (byKid[kid] || null) : null;
    };
}

// ── Route handlers ─────────────────────────────────────────────────

function computeRedirectUri(req, providerName) {
    const proto = req.headers["x-forwarded-proto"] || "http";
    const host  = req.headers["x-forwarded-host"]
                  || req.headers.host || "localhost";
    return proto + "://" + host
        + _state.callbackPath.replace("{provider}", providerName);
}

function handleLogin(req, res) {
    const providerName = req.params && req.params.provider;
    const cfg = providerName ? _state.providers[providerName] : null;
    if (!cfg) { res.status(404).html("unknown provider"); return; }

    const [verifier, challenge] = pkcePair();
    const stateValue = randomUrlsafe(16);
    const nonceValue = randomUrlsafe(16);
    const returnTo = safeReturnTo(req.query && req.query.return_to);

    // Bind envelope to this provider so a cookie minted for
    // /auth/microsoft/login can't be replayed against /auth/google/callback.
    const signed = signState({
        provider:  providerName,
        verifier:  verifier,
        state:     stateValue,
        nonce:     nonceValue,
        return_to: returnTo,
    });
    res.header("Set-Cookie", cookie.serialize(
        _state.stateCookie, signed,
        { httpOnly: true,
          sameSite: _state.stateCookieSameSite,
          // SameSite=None mandates Secure per spec, and browsers
          // silently drop None-without-Secure. Set both together.
          secure:   (_state.stateCookieSameSite === "None"),
          path:     _state.stateCookiePath,
          maxAge:   _state.stateTtl }));

    const params = {
        client_id:     cfg.clientId,
        redirect_uri:  computeRedirectUri(req, providerName),
        response_type: "code",
        scope:         cfg.scopes.join(" "),
        state:         stateValue,
        nonce:         nonceValue,
        code_challenge:        challenge,
        code_challenge_method: "S256",
    };
    res.redirect(buildUrl(cfg.authorizationEndpoint, params));
}

async function handleCallback(req, res) {
    const providerName = req.params && req.params.provider;
    const cfg = providerName ? _state.providers[providerName] : null;
    if (!cfg) { res.status(404).html("unknown provider"); return; }

    // Clear the state cookie unconditionally on every callback,
    // success or failure. State cookies are single-use by design;
    // a failed callback (state verify failure, token exchange
    // 502, id_token verify failure, etc.) used to leave the
    // cookie alive for stateTtl (600s default), giving an
    // attacker a window to spam failure paths and pin the
    // victim's cookie at a known value. Cleared here covers
    // every branch below.
    res.header("Set-Cookie", cookie.clear(_state.stateCookie,
                              { path: _state.stateCookiePath }));

    // 1. Read + verify state cookie.
    const cookies = cookie.parse(req.headers.cookie || "");
    const [env, err] = verifyState(cookies[_state.stateCookie]);
    if (!env) {
        log.warn("oauth: state verify failed: " + String(err));
        res.status(400).html("auth failed"); return;
    }
    if (env.provider !== providerName) {
        res.status(400).html("auth failed"); return;
    }

    // 2. Query state.
    const q = req.query || {};
    if (q.error) {
        log.warn("oauth: provider returned error: " + String(q.error));
        res.status(400).html("auth failed"); return;
    }
    if (!q.code || q.state !== env.state) {
        res.status(400).html("auth failed"); return;
    }

    // 3. Token exchange.
    const redirectUri = computeRedirectUri(req, providerName);
    const bodyParams = {
        grant_type:    "authorization_code",
        code:          q.code,
        redirect_uri:  redirectUri,
        client_id:     cfg.clientId,
        code_verifier: env.verifier,
    };
    if (cfg.clientSecret) bodyParams.client_secret = cfg.clientSecret;
    const bodyParts = [];
    for (const k in bodyParams) {
        if (Object.prototype.hasOwnProperty.call(bodyParams, k)) {
            bodyParts.push(urlenc(k) + "=" + urlenc(bodyParams[k]));
        }
    }
    const resp = await httpClient.async.post(cfg.tokenEndpoint,
        bodyParts.join("&"),
        { headers: {
            "Content-Type": "application/x-www-form-urlencoded",
            "Accept": "application/json",
        }});
    if (!resp || resp.status !== 200 || !resp.body) {
        log.warn("oauth: token exchange failed: " +
                 String(resp && resp.status));
        res.status(502).html("auth failed"); return;
    }
    let tokens;
    try { tokens = json.decode(resp.body); }
    catch (_e) { res.status(502).html("auth failed"); return; }
    if (!tokens || typeof tokens.id_token !== "string") {
        res.status(502).html("auth failed"); return;
    }

    // 4. Verify ID token. HS256 excluded - OIDC IdPs use asym.
    const resolver = jwksResolver(providerName);
    // jwt.verify's resolver is called synchronously in our codepath
    // (jwt.verify is sync). To support async JWKS fetch we pre-warm
    // the cache by attempting one refresh before verify.
    if (!_state._jwksCache[providerName]) {
        await refreshJwks(providerName);
    }
    const syncResolver = (kid, _alg) => {
        const cache = _state._jwksCache[providerName];
        return (cache && cache.byKid[kid]) ? cache.byKid[kid] : null;
    };
    let claims, jerr;
    [claims, jerr] = jwt.verify(tokens.id_token, syncResolver,
        { algs: ["RS256", "RS384", "RS512", "PS256", "ES256", "ES384"] });
    if (!claims) {
        // Cache miss on a rotated kid? Re-fetch and retry once.
        await refreshJwks(providerName);
        [claims, jerr] = jwt.verify(tokens.id_token, syncResolver,
            { algs: ["RS256", "RS384", "RS512", "PS256", "ES256", "ES384"] });
    }
    if (!claims) {
        log.warn("oauth: id_token verify failed: " + String(jerr));
        res.status(400).html("auth failed"); return;
    }

    // 5. OIDC claim checks beyond signature + exp.
    // Round-10 HIGH-3: multi-tenant presets fall back to a regex
    // pattern when strict equality misses. See Lua sibling.
    let issOk = (claims.iss === cfg.issuer);
    if (!issOk && cfg.issuerPattern instanceof RegExp
        && typeof claims.iss === "string") {
        issOk = cfg.issuerPattern.test(claims.iss);
    }
    if (!issOk) {
        log.warn("oauth: iss mismatch: " + String(claims.iss));
        res.status(400).html("auth failed"); return;
    }
    let audOk = false;
    if (typeof claims.aud === "string") {
        audOk = (claims.aud === cfg.clientId);
    } else if (Array.isArray(claims.aud)) {
        for (const a of claims.aud) {
            if (a === cfg.clientId) { audOk = true; break; }
        }
    }
    if (!audOk) { res.status(400).html("auth failed"); return; }
    if (claims.nonce !== env.nonce) {
        res.status(400).html("auth failed"); return;
    }

    // (State cookie was cleared at the top of the handler — every
    // callback consumes the cookie regardless of outcome.)

    // 7. Resolve claims -> app's user via findUser, then hand off
    //    via onLogin(req, res, user, ctx). The signature now
    //    matches hull/web/auth-flows so a single
    //    session.loginHandler(cookie) wires both. OIDC context
    //    (provider, claims, tokens) is in the 4th `ctx` arg.
    let target = env.return_to || "/";
    if (_state.onLogin) {
        const user = await _state.findUser(providerName, claims);
        if (!user) {
            res.status(400).html("auth failed: user resolution returned nil");
            return;
        }
        const ctx = { provider: providerName, claims, tokens };
        const v = await _state.onLogin(req, res, user, ctx);
        if (typeof v === "string") target = v;
    }
    res.redirect(target);
}

async function handleLogout(req, res) {
    res.header("Set-Cookie", cookie.clear(_state.stateCookie,
                              { path: _state.stateCookiePath }));
    let target = "/";
    if (_state.onLogout) {
        const v = await _state.onLogout(req, res);
        if (typeof v === "string") target = v;
    }
    res.redirect(target);
}

// ── Public API ─────────────────────────────────────────────────────

function init(opts) {
    if (!opts || typeof opts !== "object") {
        throw new Error("oauth.init: opts object required");
    }
    if (typeof opts.stateSecret !== "string" || opts.stateSecret.length < 32) {
        throw new Error("oauth.init: stateSecret must be a string >= 32 bytes "
            + "(same HMAC primitive as hull/web/auth-flows; pick one floor)");
    }
    _state.stateSecretHex = bytesToHex(opts.stateSecret);
    _state.stateCookie = opts.stateCookie || _state.stateCookie;
    _state.stateCookiePath =
        opts.stateCookiePath || _state.stateCookiePath;
    if (opts.stateCookieSameSite !== undefined) {
        const s = opts.stateCookieSameSite;
        if (s !== "Lax" && s !== "Strict" && s !== "None") {
            throw new Error("oauth.init: stateCookieSameSite must be "
                + "'Lax', 'Strict', or 'None'");
        }
        _state.stateCookieSameSite = s;
    }
    _state.stateTtl    = opts.stateTtl    || _state.stateTtl;
    if (opts.onLogin != null && typeof opts.findUser !== "function") {
        throw new Error("oauth.init: findUser(provider, claims) -> user is "
                        + "required when onLogin is set. See module docs.");
    }
    _state.findUser    = opts.findUser    || null;
    _state.onLogin     = opts.onLogin     || null;
    _state.onLogout    = opts.onLogout    || null;

    if (!opts.providers || typeof opts.providers !== "object") {
        throw new Error("oauth.init: providers object required");
    }
    _state.providers = {};
    for (const name in opts.providers) {
        if (!Object.prototype.hasOwnProperty.call(opts.providers, name)) continue;
        const p = opts.providers[name];
        if (!p || typeof p !== "object") {
            throw new Error("oauth.init: provider '" + name + "' must be an object");
        }
        let resolved = {};
        if (p.preset) {
            const fn = PRESETS[p.preset];
            if (!fn) {
                throw new Error("oauth.init: unknown preset '" + p.preset +
                                "' (known: google, microsoft)");
            }
            resolved = fn(p);
        }
        for (const k of ["authorizationEndpoint", "tokenEndpoint",
                          "jwksUri", "issuer"]) {
            if (p[k]) resolved[k] = p[k];
        }
        for (const k of ["authorizationEndpoint", "tokenEndpoint",
                          "jwksUri", "issuer"]) {
            if (typeof resolved[k] !== "string") {
                throw new Error("oauth.init: provider '" + name +
                                "' missing " + k +
                                " (use a preset or supply explicitly)");
            }
        }
        if (typeof p.clientId !== "string") {
            throw new Error("oauth.init: provider '" + name + "' missing clientId");
        }
        resolved.clientId     = p.clientId;
        resolved.clientSecret = p.clientSecret || null;
        resolved.scopes       = p.scopes || ["openid", "profile", "email"];
        if (!Array.isArray(resolved.scopes)) {
            throw new Error("oauth.init: provider '" + name +
                            "' scopes must be an array");
        }
        _state.providers[name] = resolved;
    }
}

function routes(app) {
    if (!_state.stateSecretHex) {
        throw new Error("oauth.routes: oauth.init() must be called first");
    }
    app.get(_state.loginPath.replace("{provider}", ":provider"), handleLogin);
    app.get(_state.callbackPath.replace("{provider}", ":provider"), handleCallback);
    app.get(_state.logoutPath, handleLogout);
}

// Test helpers (not public surface).
const _test = {
    pkcePair,
    signState,
    verifyState,
    b64ToB64url,
    base64urlToBytes,
    refreshJwks,
    safeReturnTo,
    reset: () => {
        _state.stateSecretHex = null;
        _state.providers      = {};
        _state.findUser       = null;
        _state.onLogin        = null;
        _state.onLogout       = null;
        _state._jwksCache     = {};
    },
};

const oauth = { init, routes, _test };
export { oauth };
