-- OIDC client fixture for tests/e2e_oauth.sh.
--
-- A minimal hull app that wires up hull/web/middleware/oauth against
-- the mock IdP (whose URL is passed via MOCK_IDP_ISSUER). The
-- on_login handler stuffs the verified claims into a fake session
-- and 302s to "/" - the e2e script then hits "/me" to assert the
-- claim shape made it through.

app.manifest({
    name = "oauth-test-client",
    modules = {
        "hull/web/middleware/oauth@1",
        "hull/web/cookie@1",
        "hull/log@1",
        "hull/json@1",
    },
    -- Allow the mock IdP host. The manifest takes hostnames, not
    -- ports - 127.0.0.1 covers any port.
    hosts = { "127.0.0.1" },
})

local oauth  = require("hull.web.middleware.oauth")
local cookie = require("hull.web.cookie")

-- The e2e orchestrator (tests/e2e_oauth.sh) sed-substitutes these
-- placeholders before running hull dev. We can't read env at module
-- load time because env_cfg is wired AFTER the manifest is extracted,
-- which is AFTER this top-level code runs. Template substitution
-- sidesteps the chicken-and-egg.
local issuer = "__MOCK_IDP_ISSUER__"
local secret = "__OAUTH_STATE_SECRET__"

-- In-memory "session" - a single global slot so the test can assert
-- on whatever the on_login callback recorded. Real apps would issue
-- their own signed session cookie + DB row.
local LOGGED_IN = nil

oauth.init({
    state_secret = secret,
    providers = {
        -- "test" matches the URL the e2e script uses:
        -- /auth/test/login -> /auth/test/callback.
        test = {
            authorization_endpoint = issuer .. "/authorize",
            token_endpoint         = issuer .. "/token",
            jwks_uri               = issuer .. "/.well-known/jwks.json",
            issuer                 = issuer,
            client_id              = "test-client",
            scopes                 = { "openid", "profile", "email" },
        },
    },
    find_user = function(_provider, claims)
        -- A real app would look up / create. The fixture just
        -- shapes the claims into the user object on_login expects.
        return {
            id    = claims.sub,
            email = claims.email,
            name  = claims.name,
        }
    end,
    on_login = function(_req, res, user, ctx)
        LOGGED_IN = {
            provider = ctx.provider,
            sub      = user.id,
            email    = user.email,
            name     = user.name,
        }
        res:header("Set-Cookie", cookie.serialize(
            "fixture_session", user.id,
            { path = "/", httponly = true, samesite = "Lax" }))
        return "/me"
    end,
    on_logout = function(_req, res)
        LOGGED_IN = nil
        res:header("Set-Cookie",
            cookie.clear("fixture_session", { path = "/" }))
        return "/"
    end,
})

oauth.routes(app)

app.get("/", function(_req, res)
    res:html("oauth-test-client")
end)

app.get("/me", function(_req, res)
    if not LOGGED_IN then
        return res:status(401):json({ error = "not_logged_in" })
    end
    res:json(LOGGED_IN)
end)
