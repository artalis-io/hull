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
 *     response from the IdP - including access_token,
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
 *       secret: env.get("OAUTH_STATE_SECRET"),   // alias: stateSecret
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
    stateSecret: null,
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
    providers:      Object.create(null),  // name -> resolved cfg (no prototype:
                                          // the name comes from the route)
    // findUser(provider, claims) -> user-object - required when
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
    _jwksCache:     Object.create(null),  // name -> { fetchedAt, byKid: { kid: pem } }
    // TTL on cached JWKS in seconds (forces a refresh past this age
    // even if the kid is still present). Default 1 hour. IdPs may
    // rotate a key while reusing the same kid for emergency
    // revocation; without a TTL the stale PEM would verify forever.
    jwksTtl:        3600,
    // redirect_uri origin resolution (see computeRedirectUri):
    //   baseUrl set        -> use it verbatim (recommended).
    //   trustProxy = true  -> honor X-Forwarded-Proto/Host (behind a trusted
    //                         proxy that sets them).
    //   otherwise (default)-> Host header + https; ignore forwarded headers so
    //                         a directly-exposed app can't have the redirect_uri
    //                         sent to the IdP poisoned via a spoofed header.
    baseUrl:        null,
    trustProxy:     false,
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

import { encoding } from "hull:encoding";

// An RSA JWK with no certificate - just the modulus `n` and exponent `e`,
// base64url (RFC 7518 section 6.3.1) - as an SPKI PEM. Google's JWKS is all
// such keys: x5c-only parsing skipped every one, so no Google ID token could
// ever verify. The DER is SEQUENCE { SEQUENCE { rsaEncryption, NULL },
// BIT STRING { SEQUENCE { INTEGER n, INTEGER e } } }. Byte strings throughout.
const RSA_OID = "\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01";   // 1.2.840.113549.1.1.1
function der(tag, body) {
    const n = body.length;
    let len;
    if (n < 0x80) len = String.fromCharCode(n);
    else if (n < 0x100) len = "\x81" + String.fromCharCode(n);
    else len = "\x82" + String.fromCharCode(n >> 8, n & 0xff);
    return String.fromCharCode(tag) + len + body;
}
function derUint(b) {
    b = b.replace(/^\x00+/, "");
    if (b === "" || b.charCodeAt(0) >= 0x80) b = "\x00" + b;
    return der(0x02, b);
}
function rsaJwkPem(k) {
    if (k.kty !== "RSA" || typeof k.n !== "string" || typeof k.e !== "string")
        return null;
    const n = encoding.base64.decode(k.n, { url: true });
    const e = encoding.base64.decode(k.e, { url: true });
    // 8192-bit keys at most, so every length fits two bytes.
    if (n === null || e === null || n.length < 128 || n.length > 1024
        || e.length === 0 || e.length > 8)
        return null;
    const spki = der(0x30, der(0x30, RSA_OID + "\x05\x00")
        + der(0x03, "\x00" + der(0x30, derUint(n) + derUint(e))));
    const b64 = encoding.base64.encode(spki);
    const lines = [];
    for (let i = 0; i < b64.length; i += 64) lines.push(b64.slice(i, i + 64));
    return "-----BEGIN PUBLIC KEY-----\n" + lines.join("\n")
        + "\n-----END PUBLIC KEY-----\n";
}

function randomUrlsafe(nBytes) {
    return crypto.randomToken(nBytes);
}

// PKCE per RFC 7636: verifier is 32 random bytes (~43 base64url chars),
// challenge = base64url(SHA-256(verifier)).
function pkcePair() {
    const verifier = randomUrlsafe(32);
    const challenge = encoding.base64.encode(crypto.sha256(verifier), { url: true });
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
    // Any control character or backslash: browsers drop tab/CR/LF from
    // URLs, so "/\t/evil.com" became "//evil.com" - an open redirect.
    if (/[\x00-\x1f\x7f\\]/.test(s)) return "/";
    if (s.charAt(0) !== "/") return "/";
    const c2 = s.charAt(1);
    if (c2 === "/" || c2 === "\\") return "/";
    return s;
}

function urlenc(s) {
    return encoding.url.encode(String(s));
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

// ── State cookie ───────────────────────────────────────────────────
//
// Payload: { provider, verifier, state, nonce, return_to, exp }.
// Signature framing comes from hull:crypto:envelope; the expiry
// check is OAuth-specific and stays here.

function signState(payload) {
    payload.exp = time.now() + _state.stateTtl;
    return envelope.sign(payload, _state.stateSecret);
}

function verifyState(cookieValue) {
    if (!cookieValue) return [null, "empty"];
    const r = envelope.verify(cookieValue, _state.stateSecret);
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
    const byKid = Object.create(null);   // kid comes from the token
    for (const k of doc.keys) {
        if (k && typeof k.kid === "string" && Array.isArray(k.x5c)
            && typeof k.x5c[0] === "string") {
            // x5c is standard base64 (RFC 7517 section 4.7).
            const cert = encoding.base64.decode(k.x5c[0], { lenient: true });
            if (cert !== null) {
                const pem = crypto.x509PubkeyPem(encoding.bytes.toU8(cert).buffer);
                if (pem) byKid[k.kid] = pem;
            }
        } else if (k && typeof k.kid === "string") {
            const pem = rsaJwkPem(k);
            if (pem) byKid[k.kid] = pem;
        }
    }
    _state._jwksCache[providerName] = { fetchedAt: time.now(), byKid };
    return byKid;
}

// Seconds between JWKS fetches caused by an unknown kid.
const JWKS_MIN_REFRESH = 60;

function jwksResolver(providerName) {
    return async (kid, _alg) => {
        const cache = _state._jwksCache[providerName];
        const ttl   = _state.jwksTtl || 3600;
        const fresh = cache
                      && (time.now() - (cache.fetchedAt || 0)) < ttl;
        if (fresh && cache.byKid[kid]) return cache.byKid[kid];
        // A kid still unknown within a minute of the last fetch stays
        // unknown: the key set was just read. Refetching for every such
        // token let anyone make the server fetch the IdP's JWKS once per
        // request.
        if (cache && !cache.byKid[kid]
            && (time.now() - (cache.fetchedAt || 0)) < JWKS_MIN_REFRESH) {
            return null;
        }
        // Stale OR unknown kid: refresh once. A still-unknown kid
        // after refresh returns null; signature verify then fails.
        const byKid = await refreshJwks(providerName);
        return byKid ? (byKid[kid] || null) : null;
    };
}

// ── Route handlers ─────────────────────────────────────────────────

function computeRedirectUri(req, providerName) {
    const path = _state.callbackPath.replace("{provider}", providerName);
    if (_state.baseUrl) return _state.baseUrl + path;
    let proto, host;
    if (_state.trustProxy) {
        proto = req.headers["x-forwarded-proto"] || "https";
        // X-Forwarded-Host can be a chain ("a.com, lb.internal"); take the
        // client-facing (first) hop.
        const xfh = req.headers["x-forwarded-host"];
        host = (xfh ? String(xfh).split(",")[0].trim() : "")
               || req.headers.host || "localhost";
    } else {
        // Naked case: no baseUrl, no trusted proxy. The Host header is
        // client-controllable, so the redirect_uri host is only as trustworthy
        // as the deployment. Default proto http (dev-friendly); production
        // should set baseUrl. The init warning surfaces this.
        proto = "http";
        host  = req.headers.host || "localhost";
    }
    return proto + "://" + host + path;
}

// Is the app served over https (the scheme its redirect_uri is built with)?
function stateCookieSecure(req, providerName) {
    if (_state.stateCookieSecure !== undefined) return _state.stateCookieSecure;
    if (_state.stateCookieSameSite === "None") return true;
    return computeRedirectUri(req, providerName).startsWith("https://");
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
          // The cookie carries the PKCE verifier and the nonce: Secure
          // whenever the app is served over https (and always with
          // SameSite=None, which browsers drop without it). Plain-http dev
          // keeps working; stateCookieSecure overrides.
          secure:   stateCookieSecure(req, providerName),
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
    // Constant-time: the state is the CSRF secret of this flow.
    if (!q.code || typeof q.state !== "string" || typeof env.state !== "string"
        || !crypto.constantTimeEq(q.state, env.state)) {
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
        { algs: ["RS256", "RS384", "RS512", "PS256", "ES256", "ES384"],
          requireExp: true });   // OIDC Core 3.1.3.7: an id_token expires
    if (!claims) {
        // Cache miss on a rotated kid? Re-fetch and retry once.
        await refreshJwks(providerName);
        [claims, jerr] = jwt.verify(tokens.id_token, syncResolver,
            { algs: ["RS256", "RS384", "RS512", "PS256", "ES256", "ES384"],
          requireExp: true });   // OIDC Core 3.1.3.7: an id_token expires
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

    // (State cookie was cleared at the top of the handler - every
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
    // Logout changes state, so another site must not trigger it with an
    // <img src="/auth/logout"> or a link: refuse a request the browser marks
    // as cross-site. (Same-site, same-origin, a typed URL - "none" - and
    // non-browser clients that send no header pass.)
    const site = req.headers && req.headers["sec-fetch-site"];
    if (site === "cross-site") { res.status(403).html("forbidden"); return; }
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
    // Canonical `secret`; back-compat alias `stateSecret` (same HMAC key).
    const secret = opts.secret || opts.stateSecret;
    if (typeof secret !== "string" || secret.length < 32) {
        throw new Error("oauth.init: secret must be a string >= 32 bytes "
            + "(same HMAC primitive as hull/web/auth-flows; pick one floor)");
    }
    // A byte string, like the Lua side: bytes.toU8 keeps its bytes.
    _state.stateSecret = encoding.bytes.toU8(secret);
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
    if (opts.stateCookieSecure !== undefined)
        _state.stateCookieSecure = opts.stateCookieSecure === true;
    _state.stateTtl    = opts.stateTtl    || _state.stateTtl;
    _state.jwksTtl     = opts.jwksTtl     || _state.jwksTtl;
    // redirect_uri origin. Preferred: an explicit baseUrl ("https://app.com").
    // Otherwise the origin is derived from the request; trustProxy gates
    // whether the (spoofable) X-Forwarded-Proto/Host headers are honored.
    if (opts.baseUrl != null) {
        _state.baseUrl = String(opts.baseUrl).replace(/\/+$/, "");
    }
    _state.trustProxy = opts.trustProxy === true;
    if (!_state.baseUrl && !_state.trustProxy) {
        log.warn("oauth: neither baseUrl nor trustProxy set; the redirect_uri "
            + "sent to the IdP is built from the client-controllable Host "
            + "header. Set baseUrl: \"https://app.example.com\" in production "
            + "(or trustProxy: true behind a proxy that sets X-Forwarded-Host).");
    }
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
    _state.providers = Object.create(null);
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
    if (!_state.stateSecret) {
        throw new Error("oauth.routes: oauth.init() must be called first");
    }
    app.get(_state.loginPath.replace("{provider}", ":provider"), handleLogin);
    app.get(_state.callbackPath.replace("{provider}", ":provider"), handleCallback);
    app.get(_state.logoutPath, handleLogout);
    app.post(_state.logoutPath, handleLogout);
}

// Test helpers (not public surface).
const _test = {
    pkcePair,
    signState,
    verifyState,
    refreshJwks,
    safeReturnTo,
    reset: () => {
        _state.stateSecret = null;
        _state.providers      = Object.create(null);
        _state.findUser       = null;
        _state.onLogin        = null;
        _state.onLogout       = null;
        _state._jwksCache     = Object.create(null);
    },
};

const oauth = { init, routes, _test };
export { oauth };
