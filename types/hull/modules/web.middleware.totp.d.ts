// hull:web:middleware:totp - time-based one-time passwords (RFC 6238) with recovery codes.
declare module "hull:web:middleware:totp" {
    /** An encryption key: 32 bytes (a byte string or buffer), or a `crypto.keyFromEnv` key object. */
    export type TotpKey = string | ArrayBuffer | ArrayBufferView | object;
    /** Options for `totp.init()`. */
    export interface TotpInitOptions {
        /** Issuer shown in authenticator apps. */
        issuer?: string;
        /** Code length (default 6). */
        digits?: 6 | 8;
        /** Step length in seconds (default 30). */
        period?: number;
        /** Steps accepted either side of now, 0..10 (default 1). */
        window?: number;
        /** Recovery codes minted at enrolment, 0..32 (default 10). */
        recoveryCodes?: number;
        /** One encryption key (shorthand for `encryptionKeys: {1: key}, current: 1`). */
        encryptionKey?: TotpKey;
        /** Encryption keys by version id (for rotation). */
        encryptionKeys?: { [id: number]: TotpKey };
        /** The version id used for new encryptions (required with `encryptionKeys`). */
        current?: number;
        /** The version id of the key pre-versioning rows were sealed with. */
        legacyKeyVersion?: number;
        /** Accept secrets sealed without their account binding (default true). */
        allowUnboundSecrets?: boolean;
        /** Accept plaintext rows when encryption is configured (default true). */
        allowPlaintextSecrets?: boolean;
        /** Failed verifies before a user lockout. */
        maxFailedAttempts?: number;
        /** Failed verifies per IP before an IP lockout. */
        maxFailedAttemptsPerIp?: number;
        /** User lockout length, seconds. */
        lockoutDuration?: number;
        /** Cap of an escalating lockout, seconds (0 = no escalation). */
        maxLockoutDuration?: number;
        /** IP lockout length, seconds. */
        lockoutDurationPerIp?: number;
        /** Called when a user is locked out. */
        onLockout?: (userId: string, lockedUntil: number, failures: number) => unknown;
        /** Take the client IP from X-Forwarded-For (only behind your own proxy). */
        trustXff?: boolean;
        /** Lifetime of an unconfirmed enrolment, seconds; false keeps them. */
        pendingTtl?: number | false;
        /** Schedule a daily cleanup with `app.daily` (default true). */
        cleanup?: boolean;
        /** UTC time of the daily cleanup (default `"03:00"`). */
        cleanupAt?: string;
    }
    /** What `totp.enroll()` returns: show once, then confirm. */
    export interface TotpEnrollment {
        /** The secret, base32. */
        secretBase32: string;
        /** The `otpauth://` URL. */
        otpauthUrl: string;
        /** A QR code of the URL, SVG markup. */
        qrSvg: string;
        /** One-time recovery codes. */
        recoveryCodes: string[];
    }
    /** What `totp.rekeyStatus()` returns: row counts by key version id, plus totals. */
    export interface TotpRekeyStatus {
        /** All rows. */
        total: number;
        /** Rows sealed without their account binding. */
        unbound: number;
        /** Plaintext rows. */
        plaintext: number;
        /** Rows per key version id (0 = plaintext or undecryptable). */
        [version: number]: number;
    }
    /** The `totp` module object. */
    export interface TotpModule {
        /** Configure and create the tables; call before any other function. */
        init(opts?: TotpInitOptions): void;
        /** Start an enrolment (pending until `confirm`). */
        enroll(userId: string | number): TotpEnrollment;
        /** Confirm a pending enrolment with a code; true on success. */
        confirm(userId: string | number, code: string): boolean;
        /** Verify a TOTP or recovery code; pass `req` for the per-IP lockout. */
        verify(userId: string | number, code: string, req?: Hull.Request): boolean;
        /** Verify and say which kind matched: `[true, "totp" | "recovery"]` or `[false, null]`. */
        verifyWithKind(userId: string | number, code: string, req?: Hull.Request): [true, "totp" | "recovery"] | [false, null];
        /** Re-encrypt every secret under the current key. Throws without `encryptionKeys`. */
        rekey(): { scanned: number; rekeyed: number; failed: number };
        /** Count rows by key version (read-only). */
        rekeyStatus(): TotpRekeyStatus;
        /** Remove the user's enrolment (confirmed and pending); true when something was removed. */
        disable(userId: string | number): boolean;
        /** Whether the user has a confirmed enrolment. */
        enrolled(userId: string | number): boolean;
        /** Prune stale pending enrolments and attempt rows; returns the pending enrolments removed. */
        cleanup(): number;
        /** Seconds the user stays locked out (0 when not). */
        lockoutRemaining(userId: string | number): number;
    }
    /** TOTP second factor. */
    export const totp: TotpModule;
}
