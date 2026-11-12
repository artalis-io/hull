// hull:jwt - JWT sign (HS256) / verify (HS256, RS*, PS256, ES*) / decode.
declare module "hull:jwt" {
    /** The `alg` values `jwt.verify` understands. */
    export type JwtAlg = "HS256" | "RS256" | "RS384" | "RS512" | "PS256" | "ES256" | "ES384";

    /** A decoded claim set. */
    export type JwtPayload = { [claim: string]: Hull.Json };

    /** A key resolver: the key (HMAC secret or PEM) for the token's `kid` header and `alg`; falsy fails the verify. */
    export type JwtKeyResolver = (kid: Hull.Json | undefined, alg: JwtAlg) => string | null | undefined | false;

    /** Options of `jwt.verify`. */
    export interface JwtVerifyOptions {
        /** Acceptable algs (default `["HS256"]`); must name every asymmetric alg you accept. */
        algs?: readonly JwtAlg[];
        /** Refuse a token with no `exp` claim. */
        requireExp?: boolean;
        /** Lua-parity alias of `requireExp`. */
        require_exp?: boolean;
        /** Required issuer: the `iss` claim must equal it. */
        iss?: string;
        /** Required audience, or a list of acceptable ones. */
        aud?: string | readonly string[];
        /** Seconds of clock skew allowed on `exp` / `nbf` (default 0). */
        leeway?: number;
    }

    /** JSON Web Tokens. */
    export const jwt: {
        /** An HS256 token of `payload` (an object; `iat` added, an `exp` below 1e9 taken as seconds from now) under a byte-string secret. */
        readonly sign: (payload: object, secret: string) => string;
        /** `[payload, null]` for a valid token, `[null, reason]` otherwise; throws when no key is given. */
        readonly verify: (token: string, keyOrResolver: string | JwtKeyResolver, opts?: JwtVerifyOptions | null) =>
            [payload: JwtPayload, error: null] | [payload: null, error: string];
        /** The payload WITHOUT verifying the signature (debug only), or null. */
        readonly decode: (token: string) => JwtPayload | null;
    };
}
