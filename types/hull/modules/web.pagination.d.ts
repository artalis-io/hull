// hull:web:pagination - offset pagination: query parsing and nav structure.
declare module "hull:web:pagination" {
    /** Options for `pagination.fromQuery`. */
    export interface PaginationQueryOptions {
        /** Rows per page when `?per_page` is absent (default 20). */
        defaultPerPage?: number;
        /** Upper bound on `?per_page` (default 100). */
        maxPerPage?: number;
    }
    /** Pagination parameters read from the request, SQL-ready. */
    export interface PaginationParams {
        /** Current page (1-indexed). */
        page: number;
        /** Rows per page. */
        per_page: number;
        /** `OFFSET` value. */
        offset: number;
        /** `LIMIT` value. */
        limit: number;
    }
    /** Options for `pagination.render` (note `per_page` but camelCase `baseUrl` / `defaultPerPage`). */
    export interface PaginationRenderOptions {
        /** Current page (1-indexed; numeric strings are coerced). */
        page?: number | string;
        /** Rows per page (default 20). */
        per_page?: number | string;
        /** Default per-page value: links omit `per_page=` when equal. */
        defaultPerPage?: number;
        /** URL the links point at (default ""; CR / LF / NUL throw). */
        baseUrl?: string;
        /** Pages shown either side of the current one (default 2). */
        window?: number;
    }
    /** A numbered page link. */
    export interface PaginationPageLink {
        /** Page number. */
        page: number;
        /** URL of the page. */
        url: string;
        /** True for the current page. */
        active: boolean;
        /** Absent on a page link. */
        ellipsis?: undefined;
    }
    /** A gap between non-adjacent page runs. */
    export interface PaginationEllipsis {
        /** Always true. */
        ellipsis: true;
    }
    /** The navigation structure `pagination.render` returns. */
    export interface PaginationNav {
        /** Total row count. */
        total: number;
        /** Page count (at least 1). */
        pages: number;
        /** Current page (clamped). */
        page: number;
        /** Rows per page. */
        per_page: number;
        /** URL of the previous page, or null. */
        prev: string | null;
        /** URL of the next page, or null. */
        next: string | null;
        /** Whether there is a previous page. */
        has_prev: boolean;
        /** Whether there is a next page. */
        has_next: boolean;
        /** False when everything fits on one page. */
        show: boolean;
        /** Page links and ellipses, in order. */
        links: (PaginationPageLink | PaginationEllipsis)[];
    }
    /** The `hull:web:pagination` module object. */
    export interface PaginationModule {
        /** Read `?page` / `?per_page` from `req.query`, clamped, with SQL `offset` / `limit`. */
        fromQuery(req: Hull.Request, opts?: PaginationQueryOptions): PaginationParams;
        /** Build the windowed navigation structure for a total row count. */
        render(total: number, opts?: PaginationRenderOptions): PaginationNav;
    }
    /** Pagination helpers. */
    export const pagination: PaginationModule;
}
