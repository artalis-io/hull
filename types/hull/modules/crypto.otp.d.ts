// hull:crypto:otp - HOTP (RFC 4226) and the TOTP time step (RFC 6238).
declare module "hull:crypto:otp" {
    /** The one-time-password functions. */
    export interface Otp {
        /** The HOTP code for a byte-string key at a non-negative safe-integer counter, `digits` long (default 6). */
        readonly hotp: (key: string, counter: number, digits?: 6 | 7 | 8) => string;
        /** The TOTP time step for unix time `now` (seconds) and `period` (default 30). */
        readonly step: (now: number, period?: number | null) => number;
    }

    /** One-time passwords. */
    export const otp: Otp;
    export default otp;
}
