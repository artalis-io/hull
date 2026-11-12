// hull:web:htmx:search - debounced search input + results container.
declare module "hull:web:htmx:search" {
    /** Options for `search.inputAttrs`. */
    export interface SearchInputOptions {
        /** URL the search request goes to. */
        url: string;
        /** `hx-target` (default "#hull-search-results"). */
        target?: string;
        /** Query parameter name (default "q"). */
        name?: string;
        /** Debounce delay in ms (default 300). */
        delayMs?: number;
        /** Request method, any case (default "get"); anything else throws. */
        method?: "get" | "post" | "GET" | "POST";
        /** Emit `hx-push-url="true"` (default false). */
        pushUrl?: boolean;
        /** CSS selector for `hx-indicator`. */
        indicator?: string;
    }
    /** Options for `search.resultsAttrs`. */
    export interface SearchResultsOptions {
        /** Container id (default "hull-search-results"). */
        id?: string;
    }
    /** The `hull:web:htmx:search` module object. */
    export interface SearchModule {
        /** Attribute string for the search `<input>`. */
        inputAttrs(opts: SearchInputOptions): string;
        /** Attribute string for the results container (`id` + `aria-live`). */
        resultsAttrs(opts?: SearchResultsOptions): string;
    }
    /** htmx search widget. */
    export const search: SearchModule;
    export default search;
}
