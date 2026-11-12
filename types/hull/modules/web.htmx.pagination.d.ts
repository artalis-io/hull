// hull:web:htmx:pagination - htmx-attributed pagination nav.
declare module "hull:web:htmx:pagination" {
    /** Options for `pagination.nav`. */
    export interface HtmxPaginationOptions {
        /** Current page (1-indexed). */
        page?: number;
        /** Rows per page. */
        perPage?: number;
        /** The default per-page value (decides whether links carry `per_page=`). */
        defaultPerPage?: number;
        /** URL the links point at (path + optional query). */
        baseUrl?: string;
        /** Pages shown either side of the current one (default 2). */
        window?: number;
        /** `hx-target` of each link (default "body"). */
        target?: string;
        /** Emit `hx-push-url="true"` (default true). */
        pushUrl?: boolean;
    }
    /** The `hull:web:htmx:pagination` module object. */
    export interface HtmxPaginationModule {
        /** Rendered `<nav>` HTML, or "" when there is only one page. */
        nav(total: number, opts?: HtmxPaginationOptions): string;
    }
    /** htmx pagination widget. */
    export const pagination: HtmxPaginationModule;
    export default pagination;
}
