// hull:web:cookie - Cookie header parsing and Set-Cookie serialization.
declare module "hull:web:cookie" {
    /** Set-Cookie attributes for `cookie.serialize` / `cookie.clear`. */
    export interface CookieOptions {
        /** Path attribute (default "/"; "" omits it). Throws on `;` or a control character. */
        path?: string;
        /** HttpOnly (default true). */
        httpOnly?: boolean;
        /** Secure (default true; set false for local HTTP dev). */
        secure?: boolean;
        /** SameSite (default "Lax"); anything else throws. */
        sameSite?: "Lax" | "Strict" | "None";
        /** Max-Age in seconds (floored; must be finite). */
        maxAge?: number | null;
        /** Domain attribute (`[A-Za-z0-9._-]+`). */
        domain?: string;
        /** Expires: an RFC 1123 date string, or Unix seconds. */
        expires?: string | number;
    }
    /** The `hull:web:cookie` module object. */
    export interface CookieModule {
        /** Parse a Cookie header into name -> value (first occurrence wins, null prototype); `{}` for empty input. */
        parse(header: string | null | undefined): { [name: string]: string };
        /** A Set-Cookie header value; throws on an invalid name, value or attribute. */
        serialize(name: string, value: string | number | boolean | null | undefined, opts?: CookieOptions): string;
        /** A Set-Cookie header value deleting the cookie (`Max-Age=0`); pass the original path / domain. */
        clear(name: string, opts?: Omit<CookieOptions, "maxAge" | "expires">): string;
    }
    /** Cookie helpers. */
    export const cookie: CookieModule;
}
