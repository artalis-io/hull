// OIDC client fixture (JS) for tests/e2e_oauth.sh.
//
// Mirrors tests/fixtures/oauth_client_lua/app.lua. See that file for
// the full design notes. The JS port differs only in:
//   - camelCase option keys (stateSecret, clientId, ...)
//   - on_login is an async function (resolves the redirect target)
//   - the "session" is a module-scoped let, not a global

import { app }    from "hull:app";
import { oauth }  from "hull:web:middleware:oauth";
import { cookie } from "hull:web:cookie";

app.manifest({
    name: "oauth-test-client-js",
    modules: [
        "hull/web/middleware/oauth@1",
        "hull/web/cookie@1",
        "hull/log@1",
        "hull/json@1",
    ],
    hosts: ["127.0.0.1"],
});

// The e2e orchestrator (tests/e2e_oauth.sh) sed-substitutes these
// placeholders before running hull dev. Same chicken-and-egg as the
// Lua fixture: env_cfg is wired AFTER manifest extraction (which
// happens AFTER this top-level runs), so reading env at load time
// would always return the default.
const issuer = "__MOCK_IDP_ISSUER__";
const secret = "__OAUTH_STATE_SECRET__";

let LOGGED_IN = null;

oauth.init({
    stateSecret: secret,
    providers: {
        test: {
            authorizationEndpoint: issuer + "/authorize",
            tokenEndpoint:         issuer + "/token",
            jwksUri:               issuer + "/.well-known/jwks.json",
            issuer:                issuer,
            clientId:              "test-client",
            scopes: ["openid", "profile", "email"],
        },
    },
    onLogin: async (_req, res, provider, claims, _tokens) => {
        LOGGED_IN = {
            provider,
            sub:   claims.sub,
            email: claims.email,
            name:  claims.name,
        };
        res.header("Set-Cookie", cookie.serialize(
            "fixture_session", claims.sub,
            { path: "/", httpOnly: true, sameSite: "Lax" }));
        return "/me";
    },
    onLogout: async (_req, res) => {
        LOGGED_IN = null;
        res.header("Set-Cookie",
            cookie.clear("fixture_session", { path: "/" }));
        return "/";
    },
});

oauth.routes(app);

app.get("/", (_req, res) => { res.html("oauth-test-client-js"); });

app.get("/me", (_req, res) => {
    if (!LOGGED_IN) {
        return res.status(401).json({ error: "not_logged_in" });
    }
    res.json(LOGGED_IN);
});
