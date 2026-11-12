// hull:web:auth-health - runtime health probes for the auth stack.
declare module "hull:web:auth-health" {
    /** Options for `authHealth.check`. */
    export interface AuthHealthCheckOptions {
        /** Include enumeration counts (sessions, events, enrolled users, roles, permissions). Default false. */
        includeCounts?: boolean;
    }
    /** Options for `authHealth.routes`. */
    export interface AuthHealthRoutesOptions extends AuthHealthCheckOptions {
        /** Required gate: must return exactly `true` (sync) to admit; other values 403, a throw 401, a Promise 500. */
        authCheck: (req: Hull.Request) => boolean;
        /** Mount path (default "/admin/auth-status"). */
        path?: string;
    }
    /** A probe that found its tables missing. */
    export interface AuthHealthMissing {
        /** false. */
        ok: false;
        /** Why the probe failed. */
        reason: string;
    }
    /** The sessions probe. */
    export type AuthHealthSessions = AuthHealthMissing | {
        /** true. */
        ok: true;
        /** Session rows (with includeCounts). */
        sessions?: number;
    };
    /** The audit-log probe. */
    export type AuthHealthAuditLog = AuthHealthMissing | {
        /** true. */
        ok: true;
        /** Daily cleanup: scheduled by audit-log, run externally (opted out), or missing. */
        cleanup: "scheduled" | "external" | "missing" | null;
        /** Whether the daily cleanup is scheduled, or null. */
        cleanup_scheduled: boolean | null;
        /** Event rows (with includeCounts). */
        events?: number;
    };
    /** The pwned (HIBP) probe. */
    export type AuthHealthPwned = AuthHealthMissing | {
        /** false only after a failed HIBP lookup. */
        ok: boolean;
        /** Lookup state. */
        status: "not_yet_checked" | "reachable" | "fail_open";
        /** Unix seconds of the last lookup, or null. */
        last_check_at: number | null;
        /** Error of the last failed lookup, or null. */
        last_error: string | null;
    };
    /** The TOTP probe. */
    export type AuthHealthTotp = AuthHealthMissing | {
        /** true. */
        ok: true;
        /** Confirmed enrolments (with includeCounts). */
        enrolled_users?: number;
    };
    /** The RBAC probe. */
    export type AuthHealthRbac = AuthHealthMissing | {
        /** Both tables present. */
        ok: boolean;
        /** Role rows (with includeCounts). */
        roles?: number;
        /** Permission rows (with includeCounts). */
        permissions?: number;
    };
    /** The result of `authHealth.check`. */
    export interface AuthHealthReport {
        /** Sessions probe. */
        sessions: AuthHealthSessions;
        /** Audit-log probe. */
        audit_log: AuthHealthAuditLog;
        /** Pwned-password probe. */
        pwned: AuthHealthPwned;
        /** TOTP probe. */
        totp: AuthHealthTotp;
        /** RBAC probe. */
        rbac: AuthHealthRbac;
        /** True when every probe is ok. */
        all_ok: boolean;
    }
    /** The `hull:web:auth-health` module object. */
    export interface AuthHealthModule {
        /** Run every probe. */
        check(opts?: AuthHealthCheckOptions): AuthHealthReport;
        /** Mount the GET status endpoint on `app`; throws without `opts.authCheck`. */
        routes(app: typeof import("hull:app").app, opts: AuthHealthRoutesOptions): void;
    }
    /** Auth-stack health probes. */
    export const authHealth: AuthHealthModule;
}
