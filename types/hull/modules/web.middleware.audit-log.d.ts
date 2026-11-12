// hull:web:middleware:audit-log - append-only sign-in / auth event log.
declare module "hull:web:middleware:audit-log" {
    /** Options for `auditLog.init()`. */
    export interface AuditLogInitOptions {
        /** Deployment-private salt for device fingerprints (required, 8+ characters). */
        fingerprintSalt: string;
        /** Retention in days, (0, 36500] (default 365). */
        retainDays?: number;
        /** Schedule a daily cleanup with `app.daily` (default true; false = you run it). */
        cleanup?: boolean;
        /** UTC time of the daily cleanup (default `"03:00"`). */
        cleanupAt?: string;
        /** Record the client IP from X-Forwarded-For (only behind your own proxy). */
        trustProxy?: boolean;
    }
    /** Options for `auditLog.record()`. */
    export interface AuditRecordOptions {
        /** IP to record instead of the request's. */
        ip?: string | null;
        /** User-Agent to record instead of the request's. */
        user_agent?: string | null;
        /** Fingerprint to record instead of the derived one (capped at 64 bytes). */
        fingerprint?: string;
        /** Session reference to record. */
        session_id?: string | null;
        /** Event metadata, JSON-encoded (dropped past 4 KiB). */
        metadata?: Hull.Json;
    }
    /** One audit event as `list` returns it. */
    export interface AuditEvent {
        /** Row id. */
        id: number;
        /** Event time, Unix seconds. */
        event_at: number;
        /** Event kind, e.g. `"login"`. */
        kind: string;
        /** Recorded IP. */
        ip: string | null;
        /** Recorded User-Agent. */
        user_agent: string | null;
        /** Device fingerprint. */
        fingerprint: string | null;
        /** Session reference. */
        session_id: string | null;
        /** Decoded metadata, or null. */
        metadata: Hull.Json;
    }
    /** One device as `listDevices` returns it. */
    export interface AuditDevice {
        /** Device fingerprint. */
        fingerprint: string;
        /** First event from it, Unix seconds. */
        first_seen: number;
        /** Last event from it, Unix seconds. */
        last_seen: number;
        /** Events from it. */
        count: number;
        /** An IP it used. */
        ip: string | null;
        /** A User-Agent it sent. */
        user_agent: string | null;
    }
    /** The `auditLog` module object. */
    export interface AuditLogModule {
        /** Create the table, set the fingerprint salt and retention; throws without a salt. */
        init(opts: AuditLogInitOptions): void;
        /** Record an event for a user (nothing when the id or kind is empty). */
        record(userId: string | number, kind: string, req: Hull.Request | null, opts?: AuditRecordOptions): void;
        /** A user's events, newest first (`limit` 1..1000, default 50; `kinds` filters). */
        list(userId: string | number, opts?: { limit?: number; kinds?: string[] }): AuditEvent[];
        /** A user's devices seen within `window_days` (default 90). */
        listDevices(userId: string | number, opts?: { window_days?: number }): AuditDevice[];
        /** Whether the request's device is new for the user within `window_days` (default 30). */
        isNewDevice(userId: string | number, req: Hull.Request, opts?: { window_days?: number }): boolean;
        /** The request's device fingerprint (salted hash of UA family + IP prefix). */
        fingerprint(req: Hull.Request): string;
        /** Delete events past the retention; returns the count. */
        cleanup(): number;
        /** Whether cleanup is scheduled (or explicitly external). */
        isCleanupScheduled(): boolean;
        /** How cleanup runs: scheduled here, external (opted out), or missing. */
        cleanupStatus(): "scheduled" | "external" | "missing";
        /** Recompute every row's fingerprint under the current salt (after a salt change). */
        recomputeFingerprints(): { scanned: number; updated: number; error?: "already_running" };
    }
    /** Sign-in audit log. */
    export const auditLog: AuditLogModule;
}
