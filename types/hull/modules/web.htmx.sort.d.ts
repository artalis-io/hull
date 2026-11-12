// hull:web:htmx:sort - sortable column headers driven by ?sort=col[:asc|desc].
declare module "hull:web:htmx:sort" {
    /** A parsed sort: the column and its direction. */
    export interface SortState {
        /** The column (one of the allowed names). */
        column: string;
        /** The direction. */
        direction: "asc" | "desc";
    }
    /** Options for `sort.parse`. */
    export interface SortParseOptions {
        /** The sortable column names (required: anything else is ignored). */
        allowed: string[];
        /** Column used when the request names no valid one. */
        default?: string;
    }
    /** Options for `sort.headerAttrs`. */
    export interface SortHeaderOptions {
        /** Base URL (the sort parameter is appended). */
        url?: string | number;
        /** `hx-target` (default "closest table"). */
        target?: string | number;
        /** `hx-swap` (omitted by default). */
        swap?: string | number;
        /** Emit `hx-push-url="true"` (default true). */
        pushUrl?: boolean;
    }
    /** The `hull:web:htmx:sort` module object. */
    export interface SortModule {
        /** Read and validate `?sort=` from the request; null when there is no valid sort. Throws without `opts.allowed`. */
        parse(req: Hull.Request, opts: SortParseOptions): SortState | null;
        /** Attribute string (htmx + aria-sort + classes) for a `<th>`. */
        headerAttrs(column: string | number, current: SortState | null | undefined, opts?: SortHeaderOptions): string;
    }
    /** htmx sort widget. */
    export const sort: SortModule;
    export default sort;
}
