// hull:web:middleware:session - server-side sessions (the `_hull_sessions` table).
declare namespace Hull {
    interface RequestContext {
        /** The session's id; set by `auth.sessionMiddleware()` (and read by csrf / session handlers). */
        session_id?: string;
        /** The loaded session data; set by `auth.sessionMiddleware()`. */
        session?: import("hull:web:middleware:session").SessionData;
    }
}

declare module "hull:web:middleware:session" {
    /** A session's data: a JSON object; `user_id` keys the session to its user. */
    export interface SessionData {
        /** The user the session belongs to (device management and revocation key on it). */
        user_id?: string | number;
        /** Any other field the app stores (JSON-decoded). */
        [key: string]: Hull.Data;
    }
    /** Session data to store: any JSON-encodable object (`user_id`, a string or integer, keys the session to its user). */
    export type SessionInput = { [key: string]: unknown };
    /** Cookie attributes: `hull:web:cookie`'s options. */
    export type CookieAttributes = import("hull:web:cookie").CookieOptions;
    /** The parts of `hull:web:cookie` the login / logout handler factories use. */
    export interface CookieCodec {
        /** Build a Set-Cookie value. */
        serialize(name: string, value: string, opts?: CookieAttributes): string;
        /** Parse a Cookie header into name -> value. */
        parse(header: string | null | undefined): { [name: string]: string };
        /** Build a Set-Cookie value that clears the cookie. */
        clear(name: string, opts?: Omit<CookieAttributes, "maxAge" | "expires">): string;
    }
    /** Options for `session.init()`. */
    export interface SessionInitOptions {
        /** Sliding lifetime in seconds (default 86400). */
        ttl?: number;
        /** Hard lifetime from creation in seconds (default 86400); false disables it. */
        absoluteTtl?: number | false;
        /** Schedule a daily cleanup with `app.daily` (default true; needs `hull/timers`). */
        cleanup?: boolean;
        /** UTC time of the daily cleanup (default `"03:00"`). */
        cleanupAt?: string;
        /** Record the client IP from X-Forwarded-For (only behind your own proxy; default false). */
        trustProxy?: boolean;
    }
    /** Options for `session.create()` / `session.rotate()`. */
    export interface SessionCreateOptions {
        /** This session's sliding lifetime in seconds (default: the module's). */
        ttl?: number;
        /** The request, to record the device's IP and User-Agent. */
        req?: Hull.Request;
    }
    /** One live session of a user, as `listForUser` reports it (never its id). */
    export interface SessionListing {
        /** The session's reference (first 32 hex of sha256(id)); pass to `destroyRef`. */
        ref: string;
        /** Whether this is the caller's own session. */
        current: boolean;
        /** Creation time, Unix seconds. */
        created_at: number;
        /** Last use, Unix seconds. */
        last_accessed: number;
        /** Recorded client IP, if any. */
        ip: string | null;
        /** Recorded User-Agent, if any. */
        user_agent: string | null;
    }
    /** A user as the login callbacks receive it. */
    export interface LoginUser {
        /** The user's id (required: the handler throws without it). */
        id?: string | number;
        /** The user's email, if any. */
        email?: string;
        /** Other fields of the app's user object. */
        [key: string]: unknown;
    }
    /** The 4th argument of an `onLogin` callback: who signed in and how. */
    export interface LoginContext {
        /** Factors used (auth-flows), e.g. `"password"` or `"password+totp"`. */
        factors?: string;
        /** OAuth provider name (oauth). */
        provider?: string;
        /** Verified ID-token claims (oauth). */
        claims?: { [key: string]: Hull.Json };
        /** The IdP's raw token response (oauth): bearer credentials, never store unencrypted. */
        tokens?: { [key: string]: Hull.Json };
        /** Other context fields. */
        [key: string]: unknown;
    }
    /** An `onLogin(req, res, user, ctx)` callback for auth-flows or oauth. */
    export type LoginCallback = (req: Hull.Request, res: Hull.Response, user: LoginUser, ctx?: LoginContext) => unknown;
    /** An `onLogout(req, res)` callback. */
    export type LogoutCallback = (req: Hull.Request, res: Hull.Response) => unknown;
    /** Options for `session.loginHandler()`. */
    export interface LoginHandlerOptions {
        /** Session cookie name (default `"hull_session"`). */
        name?: string;
        /** Cookie attributes (default `{ path: "/", httpOnly: true, sameSite: "Lax" }`). */
        cookieOpts?: CookieAttributes;
        /** Session data for a user (default `{ user_id: user.id, email: user.email }`). */
        extractData?: (user: LoginUser) => SessionInput;
        /** Write the response (default JSON `{ ok, user_id, email }`). */
        respond?: (res: Hull.Response, user: LoginUser, sid: string) => unknown;
        /** Rotate a prior session id (session-fixation defense; default true). */
        rotate?: boolean;
        /** Record the sign-in in this audit log. */
        auditLog?: import("hull:web:middleware:audit-log").AuditLogModule;
        /** Audit event kind (default `"login"`). */
        auditKind?: string;
        /** Metadata for the audit event; token / claim / password keys are scrubbed. */
        auditMetadata?: (user: LoginUser, ctx: LoginContext | undefined) => unknown;
        /** Called when the sign-in is from a new device (needs `auditLog`); errors are logged, not raised. */
        onNewDevice?: (req: Hull.Request, res: Hull.Response, user: LoginUser) => unknown;
    }
    /** Options for `session.logoutHandler()`. */
    export interface LogoutHandlerOptions {
        /** Session cookie name (default `"hull_session"`). */
        name?: string;
        /** Cookie attributes for the clearing cookie. */
        cookieOpts?: CookieAttributes;
        /** Write the response (default JSON `{ ok: true }`). */
        respond?: (res: Hull.Response) => unknown;
        /** Trust X-Forwarded-* for the same-origin check. */
        trustProxy?: boolean;
        /** Extra trusted origins for the same-origin check. */
        origins?: string[];
        /** Extra trusted hosts (`host` or `host:port`) for the same-origin check. */
        trustedHosts?: string[];
    }
    /** The `session` module object. */
    export interface SessionModule {
        /** Create the sessions table and configure TTLs; call once at startup. */
        init(opts?: SessionInitOptions): void;
        /** Create a session; returns its id (64 hex characters). */
        create(data?: SessionInput, opts?: SessionCreateOptions): string;
        /** Load a session's data and slide its expiry; null when missing or expired. */
        load(sessionId: string | null | undefined, opts?: { ttl?: number }): SessionData | null;
        /** Replace a session's data and slide its expiry; false when it does not exist. */
        update(sessionId: string, data: SessionInput, opts?: { ttl?: number }): boolean;
        /** Delete a session; false when nothing was deleted. */
        destroy(sessionId: string | null | undefined): boolean;
        /** Delete expired sessions; returns the count. */
        cleanup(): number;
        /** A user's live sessions, newest first; `current` is the caller's session id or request. */
        listForUser(userId: string | number, current?: string | Hull.Request | null): SessionListing[];
        /** Destroy one of the user's sessions by its `ref`; returns 0 or 1. */
        destroyRef(userId: string | number, ref: string): number;
        /** Destroy every session of the user except `currentSid`; returns the count. */
        destroyOthers(currentSid: string | null | undefined, userId: string | number): number;
        /** Destroy every session of the user; returns the count. */
        destroyAll(userId: string | number): number;
        /** Destroy `oldSid` (if any) and create a new session; returns the new id. */
        rotate(oldSid: string | null | undefined, data?: SessionInput, opts?: SessionCreateOptions): string;
        /** Build an `onLogin(req, res, user, ctx)` callback that creates the session and sets its cookie. Needs `init()` first. */
        loginHandler(cookieMod: CookieCodec, opts?: LoginHandlerOptions): LoginCallback;
        /** Build an `onLogout(req, res)` callback that destroys the session and clears its cookie (403 cross-site). */
        logoutHandler(cookieMod: CookieCodec, opts?: LogoutHandlerOptions): LogoutCallback;
    }
    /** Server-side sessions. */
    export const session: SessionModule;
}
