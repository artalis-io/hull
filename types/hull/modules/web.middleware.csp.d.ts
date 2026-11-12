// hull:web:middleware:csp - Content-Security-Policy middleware with a per-request nonce.
declare namespace Hull {
    interface RequestContext {
        /** The request's CSP nonce (base64url); set by `csp.htmx()` / `csp.strict()` middleware. */
        csp_nonce?: string;
    }
}

declare module "hull:web:middleware:csp" {
    /** Options for the CSP profiles. */
    export interface CspOptions {
        /** Send `Content-Security-Policy-Report-Only` instead of enforcing. */
        reportOnly?: boolean;
    }
    /** The `csp` module object. */
    export interface CspModule {
        /** Pico-compatible profile (allows inline `style=` attributes); sets `req.ctx.csp_nonce`. */
        htmx(opts?: CspOptions): Hull.Middleware;
        /** Strict profile (no inline styles); sets `req.ctx.csp_nonce`. */
        strict(opts?: CspOptions): Hull.Middleware;
        /** A fresh 128-bit base64url nonce, or null when randomness failed. */
        nonce(): string | null;
    }
    /** CSP middleware profiles. */
    export const csp: CspModule;
    /** Default export: the same `csp` object. */
    export default csp;
}
