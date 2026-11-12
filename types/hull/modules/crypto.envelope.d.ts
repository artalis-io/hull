// hull:crypto:envelope - HMAC-signed, JSON-payload, stateless tokens.
declare module "hull:crypto:envelope" {
    /** Why `envelope.verify` refused a token. */
    export type EnvelopeError = "missing" | "malformed" | "bad tag" | "bad encoding" | "bad json";

    /** A verified envelope payload: the JSON object or array that was signed. */
    export type EnvelopePayload = { [key: string]: Hull.Json } | Hull.Json[];

    /** Sign and verify `base64url(JSON) "." hex(HMAC-SHA256)` tokens. */
    export const envelope: {
        /** A token carrying `payload` (any JSON-encodable value), MACed with `secret` (a buffer, or a string as UTF-8). */
        readonly sign: (payload: unknown, secret: import("hull:crypto").CryptoBytes) => string;
        /** `[payload, null]` for a genuine token, `[null, reason]` otherwise; checks only the MAC and framing. */
        readonly verify: (token: string, secret: import("hull:crypto").CryptoBytes) =>
            [payload: EnvelopePayload, error: null] | [payload: null, error: EnvelopeError];
    };
}
