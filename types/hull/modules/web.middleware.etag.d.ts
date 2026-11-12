// hull:web:middleware:etag - response helpers with weak ETags and 304s.
declare module "hull:web:middleware:etag" {
    /** The `etag` module object. */
    export interface EtagModule {
        /** A weak ETag `W/"<16 hex>"` for a body, or null for an empty one. */
        compute(body: string | null | undefined): string | null;
        /** Whether the request's `If-None-Match` matches `tag` (a list or `*`). */
        matches(req: Hull.Request, tag: string | null | undefined): boolean;
        /** Send `data` as JSON with an ETag, or 304 when `If-None-Match` matches (GET / HEAD, status 200 only). */
        json(req: Hull.Request, res: Hull.Response, data: unknown, status?: number): void;
        /** Send text with an ETag, or 304. */
        text(req: Hull.Request, res: Hull.Response, text: string | null | undefined, status?: number): void;
        /** Send HTML with an ETag, or 304. */
        html(req: Hull.Request, res: Hull.Response, html: string | null | undefined, status?: number): void;
    }
    /** ETag response helpers (not a middleware). */
    export const etag: EtagModule;
}
