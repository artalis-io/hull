// hull:web:auth-flows - registration / verify / login / reset / magic-link / email-change routes.
declare module "hull:web:auth-flows" {
    /** A user id as the app's user model stores it. */
    export type AuthFlowsUserId = string | number;
    /** A user object returned by the app's user callbacks. */
    export interface AuthFlowsUser {
        /** The user id (canonical). */
        id?: AuthFlowsUserId;
        /** Legacy spelling of the id, read when `id` is absent. */
        user_id?: AuthFlowsUserId;
        /** The stored address. */
        email?: string;
        /** The password hash (`crypto.hashPassword`), null for a passwordless account; `userFindByEmail` must return it. */
        password_hash?: string | null;
        /** Verified flag: only `true` or a non-zero number count as verified. */
        email_verified?: boolean | number | string;
        /** Any other column of the app's model. */
        [field: string]: unknown;
    }
    /** The sanitized user handed to `onLogin` and the templates (`id`, `user_id`, `email`, `email_verified` unless `userSanitize` is set). */
    export interface AuthFlowsSafeUser {
        /** The user id. */
        id?: AuthFlowsUserId;
        /** Legacy spelling of the id. */
        user_id?: AuthFlowsUserId;
        /** The stored address. */
        email?: string;
        /** The verified flag as the model returned it. */
        email_verified?: boolean | number | string;
        /** Fields a custom `userSanitize` kept. */
        [field: string]: unknown;
    }
    /** The first factor(s) a sign-in passed. */
    export type AuthFlowsFactors = "password" | "magic_link" | "password+totp" | "magic_link+totp";
    /** The 4th argument of `onLogin`. */
    export interface AuthFlowsLoginContext {
        /** The factors this sign-in passed. */
        factors: AuthFlowsFactors;
    }
    /** What a template returns: a subject and an HTML and / or a text body. */
    export interface AuthFlowsMail {
        /** Subject line. */
        subject: string;
        /** HTML body. */
        html?: string;
        /** Plain-text body. */
        text?: string;
    }
    /** The context a template receives (fields depend on the template). */
    export interface AuthFlowsTemplateContext {
        /** The sanitized user. */
        user: AuthFlowsSafeUser;
        /** The raw token. */
        token?: string;
        /** Click-through link (magic_link, password_reset, email_change). */
        link?: string;
        /** Verification link (welcome). */
        verify_url?: string;
        /** The requested new address (email_change, email_change_notify). */
        new_email?: string;
        /** Undo link mailed to the current address (email_change_notify). */
        revoke_url?: string;
        /** Undo token (email_change_notify). */
        revoke_token?: string;
    }
    /** A mail template. */
    export type AuthFlowsTemplate = (ctx: AuthFlowsTemplateContext) => AuthFlowsMail;
    /** The mail templates, by name (a missing one throws when that mail is sent). */
    export interface AuthFlowsTemplates {
        /** Registration / verify-resend mail (`verify_url`). */
        welcome?: AuthFlowsTemplate;
        /** Magic sign-in link (`link`). */
        magic_link?: AuthFlowsTemplate;
        /** Password-reset link (`link`). */
        password_reset?: AuthFlowsTemplate;
        /** Confirmation sent to the new address (`link`, `new_email`). */
        email_change?: AuthFlowsTemplate;
        /** Notice + undo link sent to the current address; needs `onPasswordReset`. */
        email_change_notify?: AuthFlowsTemplate;
        /** Other names are accepted and unused. */
        [name: string]: AuthFlowsTemplate | undefined;
    }
    /** The six user-model callbacks in short form (`authFlows.standardUsers()` returns them). */
    export interface AuthFlowsUsers {
        /** Find by address, folding no more than ASCII case; must include `password_hash`. Synchronous. */
        findByEmail(email: string): AuthFlowsUser | null | undefined;
        /** Find by id. Synchronous. */
        get(id: AuthFlowsUserId): AuthFlowsUser | null | undefined;
        /** Create an account (hash null for passwordless); returns its id. Synchronous. */
        create(email: string, passwordHash: string | null): AuthFlowsUserId;
        /** Replace the password hash. Synchronous. */
        setPassword(id: AuthFlowsUserId, passwordHash: string): void;
        /** Replace the address. Synchronous. */
        setEmail(id: AuthFlowsUserId, email: string): void;
        /** Set the verified flag. Synchronous. */
        setEmailVerified(id: AuthFlowsUserId, verified: boolean): void;
    }
    /** Options for `authFlows.standardUsers`. */
    export interface StandardUsersOptions {
        /** Users table (default "users"; `_hull_*` names throw). */
        table?: string;
        /** Id generator for new accounts (default 16 random hex bytes). */
        idGen?: () => AuthFlowsUserId;
    }
    /** The per-IP login rate limit (`loginRatelimit`). */
    export interface AuthFlowsLoginRatelimit {
        /** Requests per window (default 20). */
        limit?: number;
        /** Window in seconds (default 300). */
        window?: number;
        /** Bucket key (default: the client IP). */
        key?: (req: Hull.Request) => string;
    }
    /** The per-recipient mail rate limit (`emailRateLimit`). */
    export interface AuthFlowsEmailRateLimit {
        /** Mails per window (default 3). */
        limit?: number;
        /** Window in seconds (default 900). */
        window?: number;
    }
    /** Options for `authFlows.init`. */
    export interface AuthFlowsInitOptions {
        /** HMAC key for the signed tokens, at least 32 bytes (or `stateSecret`). */
        secret?: string;
        /** Back-compat alias of `secret`. */
        stateSecret?: string;
        /** Sends a mail; a Promise is observed, `false` means not sent. */
        emailSend: (to: string, subject: string, html: string | undefined, text: string | undefined) => void | boolean | Promise<unknown>;
        /** Mail templates. */
        templates: AuthFlowsTemplates;
        /** The six user callbacks at once; an explicit `userX` wins. */
        users?: AuthFlowsUsers;
        /** Find by address (see `AuthFlowsUsers.findByEmail`). */
        userFindByEmail?: AuthFlowsUsers["findByEmail"];
        /** Find by id. */
        userGet?: AuthFlowsUsers["get"];
        /** Create an account. */
        userCreate?: AuthFlowsUsers["create"];
        /** Replace the password hash. */
        userSetPassword?: AuthFlowsUsers["setPassword"];
        /** Replace the address. */
        userSetEmail?: AuthFlowsUsers["setEmail"];
        /** Set the verified flag. */
        userSetEmailVerified?: AuthFlowsUsers["setEmailVerified"];
        /** Issue the session after a sign-in (e.g. `session.loginHandler(cookie)`); a Promise is awaited. */
        onLogin: (req: Hull.Request, res: Hull.Response, user: AuthFlowsSafeUser, ctx: AuthFlowsLoginContext) => unknown;
        /** End the session on POST /logout (default: redirect to "/"); a Promise is awaited. */
        onLogout?: (req: Hull.Request, res: Hull.Response) => unknown;
        /** Revoke sessions after a password reset / verification / email-change undo (e.g. `session.destroyAll(user.id)`). */
        onPasswordReset?: (req: Hull.Request, res: Hull.Response, user: AuthFlowsSafeUser) => unknown;
        /** The canonical origin emailed links use ("https://app.example.com"). One of publicOrigin / trustedHosts / trustRequestHost is required. */
        publicOrigin?: string;
        /** Allowed request hosts ("host" or "host:port"). */
        trustedHosts?: string[];
        /** Build links from the request's Host (dev / tests only). */
        trustRequestHost?: boolean;
        /** Honour X-Forwarded-Proto / -For (behind a trusted proxy only). */
        trustProxy?: boolean;
        /** Reduce a user before it reaches onLogin / templates (synchronous). */
        userSanitize?: (user: AuthFlowsUser) => AuthFlowsSafeUser;
        /** Route prefix (default "/auth"). */
        prefix?: string;
        /** Same success shape whether an address exists (default true). */
        enumerationSafe?: boolean;
        /** Magic link to an unknown address creates a passwordless account (default false). */
        magicLinkAutoSignup?: boolean;
        /** Refuse sign-in until verified (default true); false needs onPasswordReset. */
        requireVerifiedEmail?: boolean;
        /** Verify-token lifetime in seconds (default 86400). */
        verifyTtl?: number;
        /** Reset-token lifetime in seconds (default 3600). */
        resetTtl?: number;
        /** Magic-link lifetime in seconds (default 600). */
        magicLinkTtl?: number;
        /** Email-change token lifetime in seconds (default 86400). */
        emailChangeTtl?: number;
        /** Redirect after a successful verification (default "/"). */
        verifyRedirect?: string;
        /** App page rendering the verify form instead of the default (gets `?token=`). */
        verifyFormRedirect?: string | null;
        /** Redirect after a page-form sign-in (default "/"). */
        loginRedirect?: string;
        /** Wedge a TOTP step into sign-in (needs userTotpEnrolled, totpVerify, totpDisable). */
        enableTotp?: boolean;
        /** Whether the user has a confirmed TOTP enrolment (synchronous). */
        userTotpEnrolled?: (userId: AuthFlowsUserId) => boolean;
        /** Check a TOTP code; must return exactly `true` (synchronous). */
        totpVerify?: (user: AuthFlowsUser, code: string, req: Hull.Request) => boolean;
        /** Remove a TOTP enrolment (e.g. `totp.disable`); may be async. */
        totpDisable?: ((userId: AuthFlowsUserId) => unknown) | null;
        /** Pending-2FA token lifetime in seconds (default 300). */
        totpPendingTtl?: number;
        /** App page for the TOTP step (gets `?token=`). */
        totpPendingRedirect?: string;
        /** Returns `true` when the request proves a recent sign-in, so POST /email-change needs no password. */
        emailChangeReauth?: ((req: Hull.Request, user: AuthFlowsUser) => boolean) | null;
        /** Failed logins per (account, IP) before lockout (default 5). */
        maxFailedLogins?: number;
        /** Failed logins per account before lockout (default 50). */
        maxFailedLoginsPerAccount?: number;
        /** Lockout length in seconds (default 900). */
        lockoutDuration?: number;
        /** Refuse breached passwords via hull:web:pwned (default false; add the HIBP host to manifest.hosts). */
        checkPwnedPasswords?: boolean;
        /** HIBP range endpoint override. */
        pwnedEndpoint?: string;
        /** Record flow events through hull:web:middleware:audit-log (default false). */
        signInLog?: boolean;
        /** Per-IP rate limit on register / login / magic-link / reset (default off). */
        loginRatelimit?: boolean | AuthFlowsLoginRatelimit;
        /** Per-recipient mail rate limit (default `{ limit: 3, window: 900 }`); false disables. */
        emailRateLimit?: false | AuthFlowsEmailRateLimit;
        /** Daily prune of expired rows (default on); false opts out. */
        cleanup?: boolean;
        /** Time of the daily prune, "HH:MM" (default "03:00"). */
        cleanupAt?: string | null;
    }
    /** The `hull:web:auth-flows` module object. */
    export interface AuthFlowsModule {
        /** Configure the flows and create the internal tables; throws on a missing / invalid option. */
        init(opts: AuthFlowsInitOptions): void;
        /** Mount the flow routes under the prefix; call after init. */
        routes(app: typeof import("hull:app").app): void;
        /** The six user callbacks over a standard users table. */
        standardUsers(opts?: StandardUsersOptions): AuthFlowsUsers;
        /** Prune expired tokens, pending changes and lockout rows now. */
        cleanup(): void;
        /** Mail a verification link to the user (`urlPrefix` is prepended to the path). */
        sendVerifyEmail(user: AuthFlowsUser, urlPrefix?: string): void;
        /** Mail a password-reset link when the address is an account's; throws on an invalid address. */
        sendPasswordReset(email: string, urlPrefix?: string): void;
        /** Mail a magic sign-in link when the address is an account's; throws on an invalid address. */
        sendMagicLink(email: string, urlPrefix?: string): void;
        /** Whether the address is held for the undo of a confirmed email change (treat it as taken). */
        emailReserved(email: string): boolean;
        /** Clear the recovery lock of a user after a manual restore; true when one was cleared. */
        unlockRecovery(userId: AuthFlowsUserId | null | undefined): boolean;
    }
    /** Transactional auth flows. */
    export const authFlows: AuthFlowsModule;
}
