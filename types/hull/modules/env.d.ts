// hull:env - environment variables named in manifest `env`.
declare module "hull:env" {
    /** Allowlisted environment access. */
    export const env: {
        /** The variable's value, or `null` when it is unset or not in manifest `env`. */
        get(name: string): string | null;
        /** Whether `name` is in manifest `env` (set or not). */
        allowed(name: string): boolean;
    };
}
