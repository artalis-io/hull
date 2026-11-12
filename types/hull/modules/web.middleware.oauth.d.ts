// hull:web:middleware:oauth - OAuth 2.0 / OIDC authorization code flow with PKCE.
declare module "hull:web:middleware:oauth" {
    import type { LoginCallback, LoginUser } from "hull:web:middleware:session";
    /** One identity provider. */
    export interface OAuthProvider {
        /** A preset filling the endpoints. */
        preset?: "google" | "microsoft";
        /** Entra tenant for the `microsoft` preset (default `"common"`). */
        tenant?: string;
        /** The client id (required). */
        clientId: string;
        /** The client secret. */
        clientSecret?: string;
        /** Scopes (default `["openid", "profile", "email"]`). */
        scopes?: string[];
        /** Authorization endpoint (required without a preset). */
        authorizationEndpoint?: string;
        /** Token endpoint (required without a preset). */
        tokenEndpoint?: string;
        /** JWKS URI (required without a preset). */
        jwksUri?: string;
        /** Expected ID-token issuer (pins it; required without a preset). */
        issuer?: string;
    }
    /** Options for `oauth.init()`. */
    export interface OAuthInitOptions {
        /** HMAC key for the state cookie, 32+ characters (or `stateSecret`). */
        secret?: string;
        /** Back-compat alias of `secret`. */
        stateSecret?: string;
        /** Providers by name (the `:provider` route segment). */
        providers: { [name: string]: OAuthProvider };
        /** Map a verified sign-in to the app's user (required with `onLogin`). */
        findUser?: (provider: string, claims: { [key: string]: Hull.Json }) =>
            LoginUser | null | undefined | Promise<LoginUser | null | undefined>;
        /** Called after sign-in as `onLogin(req, res, user, { provider, claims, tokens })`; a returned string is the redirect target. */
        onLogin?: LoginCallback;
        /** Called on logout; a returned string is the redirect target. */
        onLogout?: (req: Hull.Request, res: Hull.Response) => unknown;
        /** State cookie name (default `"_oauth_state"`). */
        stateCookie?: string;
        /** State cookie path (default `"/auth"`). */
        stateCookiePath?: string;
        /** State cookie SameSite (default `"Lax"`). */
        stateCookieSameSite?: "Lax" | "Strict" | "None";
        /** Force the state cookie's Secure flag. */
        stateCookieSecure?: boolean;
        /** State lifetime, seconds (default 600). */
        stateTtl?: number;
        /** JWKS cache lifetime, seconds (default 3600). */
        jwksTtl?: number;
        /** The app's public origin for the redirect URI, e.g. `"https://app.example.com"` (recommended). */
        baseUrl?: string;
        /** Honour X-Forwarded-Proto / Host for the redirect URI (behind your own proxy). */
        trustProxy?: boolean;
    }
    /** The part of `app` that `oauth.routes()` registers on. */
    export interface OAuthRouteTarget {
        /** Register a GET route. */
        get(pattern: string, handler: Hull.Handler): unknown;
        /** Register a POST route. */
        post(pattern: string, handler: Hull.Handler): unknown;
    }
    /** The `oauth` module object. */
    export interface OAuthModule {
        /** Configure the providers and callbacks; throws on a bad configuration. */
        init(opts: OAuthInitOptions): void;
        /** Register `/auth/:provider/login`, `/auth/:provider/callback` and `/auth/logout`. Call `init` first. */
        routes(app: OAuthRouteTarget): void;
    }
    /** OAuth / OIDC sign-in. */
    export const oauth: OAuthModule;
}
