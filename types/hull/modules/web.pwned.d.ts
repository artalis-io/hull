// hull:web:pwned - k-anonymity pwned-password check (local blocklist + HIBP range API).
declare module "hull:web:pwned" {
    /** Options for `pwned.check`. */
    export interface PwnedCheckOptions {
        /** HIBP range endpoint (default "https://api.pwnedpasswords.com/range/"); its host must be in `manifest.hosts`. */
        endpoint?: string;
    }
    /** The state of the last HIBP lookup. */
    export interface PwnedHealth {
        /** true: last lookup answered; false: it failed (fail-open); null: none yet. */
        ok: boolean | null;
        /** Unix seconds of the last lookup, or null. */
        last_check_at: number | null;
        /** Error of the last failed lookup, or null. */
        last_error: string | null;
    }
    /** The `hull:web:pwned` module object. */
    export interface PwnedModule {
        /** True when the password is known breached; fails open (false) on an HIBP outage. Rejects with a Hull.RefusalError on a runtime refusal, or when `manifest.hosts` cannot admit the endpoint. */
        check(password: string, opts?: PwnedCheckOptions): Promise<boolean>;
        /** A copy of the last lookup's state. */
        health(): PwnedHealth;
    }
    /** Pwned-password check. */
    export const pwned: PwnedModule;
}
