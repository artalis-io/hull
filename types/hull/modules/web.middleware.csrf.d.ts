// hull:web:middleware:csrf - stateless CSRF tokens (HMAC-SHA256).
declare namespace Hull {
    interface RequestContext {
        /** A CSRF token for templates; set by `csrf.middleware()` on safe methods when a session resolves. */
        csrf_token?: string;
    }
}

declare module "hull:web:middleware:csrf" {
    /** Options for `csrf.middleware()`. */
    export interface CsrfOptions {
        /** HMAC secret (required; 32+ random bytes recommended). */
        secret: string;
        /** `req.ctx` key holding the session id (default `"session_id"`). */
        sessionKey?: string;
        /** Max token age in seconds (default 3600). */
        ttl?: number;
        /** Back-compat alias of `ttl`. */
        maxAge?: number;
        /** Methods that get a token instead of a check (default `["GET", "HEAD", "OPTIONS"]`). */
        safeMethods?: string[];
        /** Request header carrying the token (default `"X-CSRF-Token"`). */
        headerName?: string;
        /** Form field carrying the token (default `"_csrf"`). */
        fieldName?: string;
        /** Session cookie name for the session-id fallback (default `"hull_session"`). */
        name?: string;
        /** Back-compat alias of `name`. */
        cookieName?: string;
        /** Answer 403 to an unsafe request with no session instead of passing it (default false). */
        requireSession?: boolean;
    }
    /** The `csrf` module object. */
    export interface CsrfModule {
        /** Mint a token (`tsHex.hmacHex`) for a session id; throws on a missing argument. */
        generate(sessionId: string, secret: string): string;
        /** Verify a token: constant-time HMAC compare plus an age check (`ttl` seconds, default 3600). */
        verify(token: string | null | undefined, sessionId: string | null | undefined, secret: string, ttl?: number): boolean;
        /** Build the CSRF middleware (register with `app.usePost`); 403 on a missing or invalid token. */
        middleware(opts: CsrfOptions): Hull.Middleware;
    }
    /** Stateless CSRF protection. */
    export const csrf: CsrfModule;
}
