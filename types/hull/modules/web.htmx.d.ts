// hull:web:htmx - htmx request inspection and response-header helpers.
declare module "hull:web:htmx" {
    /** Options for the URL-taking helpers (`redirect`, `pushUrl`, `replaceUrl`, `location`). */
    export interface HtmxUrlOptions {
        /** Accept a protocol-relative `//host/...` target (refused by default). */
        allowProtocolRelative?: boolean;
    }
    /** Options for `htmx.trigger`. */
    export interface HtmxTriggerOptions {
        /** When the event fires: immediately (default, HX-Trigger), after the swap, or after settle. */
        timing?: "swap" | "settle";
    }
    /** An htmx `hx-swap` mode. */
    export type HtmxSwapMode =
        | "innerHTML" | "outerHTML" | "beforebegin" | "afterbegin"
        | "beforeend" | "afterend" | "delete" | "none";
    /** An htmx LocationContext for `HX-Location`. */
    export interface HtmxLocationContext {
        /** The path to navigate to (an http(s) URL or a relative path). */
        path: string;
        /** CSS selector of the swap target. */
        target?: string;
        /** Swap mode. */
        swap?: HtmxSwapMode | (string & {});
        /** Element to take the request from. */
        source?: string;
        /** The client event name. */
        event?: string;
        /** A JavaScript handler name. */
        handler?: string;
        /** Request values. */
        values?: { [name: string]: Hull.Json };
        /** Request headers. */
        headers?: { [name: string]: string };
        /** Which elements to include in the request. */
        select?: string;
    }
    /** The `hull:web:htmx` module object. */
    export interface HtmxModule {
        /** HTML-escape a value for element text or a double-quoted attribute; null / undefined give "". */
        escape(value: unknown): string;
        /** True when the request was sent by the htmx client (`HX-Request: true`). */
        is(req: Hull.Request): boolean;
        /** True when the request is an htmx boost (`HX-Boosted: true`). */
        boosted(req: Hull.Request): boolean;
        /** The page URL the htmx request came from (`HX-Current-URL`), or null. */
        currentUrl(req: Hull.Request): string | null;
        /** The id of the htmx target element (`HX-Target`), or null. */
        target(req: Hull.Request): string | null;
        /** The name of the triggering element (`HX-Trigger-Name`), or null. */
        triggerName(req: Hull.Request): string | null;
        /** Redirect: HX-Redirect + 204 for an htmx request, a plain 302 otherwise. Throws a TypeError on a non-http(s) / scheme target. */
        redirect(req: Hull.Request, res: Hull.Response, path: string, opts?: HtmxUrlOptions): void;
        /** Set HX-Retarget: apply the response to another element. */
        retarget(res: Hull.Response, selector: string): void;
        /** Set HX-Reswap: override the swap mode. */
        reswap(res: Hull.Response, mode: HtmxSwapMode | (string & {})): void;
        /** Trigger one client-side event, with an optional JSON payload. */
        trigger(res: Hull.Response, event: string, payload?: Hull.Json | null, opts?: HtmxTriggerOptions): void;
        /** Trigger several events at once: a map of event name to payload. */
        trigger(res: Hull.Response, events: { [event: string]: Hull.Json }, opts?: HtmxTriggerOptions): void;
        /** Set HX-Refresh: true (a full client-side page refresh). */
        refresh(res: Hull.Response): void;
        /** Set HX-Push-Url; `false` suppresses a push. Throws a TypeError on a non-http(s) / scheme URL. */
        pushUrl(res: Hull.Response, url: string | false, opts?: HtmxUrlOptions): void;
        /** Set HX-Replace-Url; `false` suppresses it. Throws a TypeError on a non-http(s) / scheme URL. */
        replaceUrl(res: Hull.Response, url: string | false, opts?: HtmxUrlOptions): void;
        /** Set HX-Location: a client-side navigation without a reload. Throws a TypeError on a bad path. */
        location(res: Hull.Response, pathOrContext: string | HtmxLocationContext, opts?: HtmxUrlOptions): void;
    }
    /** htmx request / response helpers. */
    export const htmx: HtmxModule;
    export default htmx;
}
