// hull:web:middleware:auth - session and JWT authentication middleware, login / logout helpers.
declare namespace Hull {
    interface RequestContext {
        /** The verified JWT payload; set by `auth.jwtMiddleware()`. */
        user?: import("hull:web:middleware:auth").JwtPayload;
    }
}

declare module "hull:web:middleware:auth" {
    import type { CookieAttributes, SessionInput } from "hull:web:middleware:session";
    /** A verified JWT payload (`req.ctx.user`). */
    export interface JwtPayload {
        /** Subject. */
        sub?: string;
        /** Expiry, Unix seconds. */
        exp?: number;
        /** Issued at, Unix seconds. */
        iat?: number;
        /** Issuer. */
        iss?: string;
        /** Audience. */
        aud?: string | string[];
        /** Other claims. */
        [claim: string]: Hull.Json | undefined;
    }
    /** Options for `auth.sessionMiddleware()`. */
    export interface SessionAuthOptions {
        /** Session cookie name (default `"hull_session"`). */
        name?: string;
        /** Back-compat alias of `name`. */
        cookieName?: string;
        /** Redirect here (302) instead of answering 401. */
        loginPath?: string;
        /** Continue without a session instead of refusing. */
        optional?: boolean;
        /** Paths skipped entirely (exact, or a `/prefix/*`). */
        excludePaths?: string[];
        /** Attributes of the cookie that clears a stale session. */
        cookieOpts?: CookieAttributes;
    }
    /** Options for `auth.jwtMiddleware()`. */
    export interface JwtAuthOptions {
        /** HMAC-SHA256 secret (required). */
        secret: string;
        /** Continue without a valid token instead of answering 401. */
        optional?: boolean;
        /** Paths skipped entirely (exact, or a `/prefix/*`). */
        excludePaths?: string[];
        /** Require an `exp` claim (default true). */
        requireExp?: boolean;
        /** Required issuer. */
        iss?: string;
        /** Required audience. */
        aud?: string;
        /** Clock skew allowed, seconds. */
        leeway?: number;
    }
    /** Options for `auth.login()`. */
    export interface LoginOptions {
        /** Session cookie name (default `"hull_session"`). */
        name?: string;
        /** Back-compat alias of `name`. */
        cookieName?: string;
        /** Cookie attributes. */
        cookieOpts?: CookieAttributes;
        /** Session lifetime in seconds (also the cookie's Max-Age when not given). */
        ttl?: number;
    }
    /** Options for `auth.logout()`. */
    export interface LogoutOptions {
        /** Session cookie name (default `"hull_session"`). */
        name?: string;
        /** Back-compat alias of `name`. */
        cookieName?: string;
        /** Attributes of the clearing cookie. */
        cookieOpts?: CookieAttributes;
    }
    /** The `auth` module object. */
    export interface AuthModule {
        /** Cookie-session auth: sets `req.ctx.session_id` / `req.ctx.session`; 401 or redirect when absent. */
        sessionMiddleware(opts?: SessionAuthOptions): Hull.Middleware;
        /** Bearer-JWT auth: sets `req.ctx.user`; 401 when invalid. Throws without `secret`. */
        jwtMiddleware(opts: JwtAuthOptions): Hull.Middleware;
        /** Create a session for `userData` and set its cookie; returns the session id. */
        login(req: Hull.Request, res: Hull.Response, userData: SessionInput, opts?: LoginOptions): string;
        /** Destroy the request's session and clear its cookie. */
        logout(req: Hull.Request, res: Hull.Response, opts?: LogoutOptions): void;
    }
    /** Authentication middleware. */
    export const auth: AuthModule;
}
