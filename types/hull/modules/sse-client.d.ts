// hull:sse-client - read a Server-Sent Events stream (the HTML standard's event-stream parser).
declare module "hull:sse-client" {
    import type { HttpStream } from "hull:http-client";
    /** One dispatched event. */
    export interface SseEvent {
        /** The event type: the server's `event` field, else "message". */
        event: string;
        /** The `data` lines, joined with LF. */
        data: string;
        /** The last event id seen on the stream, or null. */
        id: string | null;
        /** The reconnection time (ms) the server last set with a `retry` field (kept across events), or null. */
        retry: number | null;
    }
    /** Options of `parser()` and `events()`. */
    export interface SseOptions {
        /** The most one event may hold: its pending line plus its data so far, in UTF-16 code units (default 1048576). Past it the parser throws an Error with code "too_large". */
        maxEventBytes?: number;
    }
    /** The parser alone, for text from any source. */
    export interface SseParser {
        /** Feed text; returns the events it completes. Throws code "too_large" past maxEventBytes (and on every later call). */
        feed(text: string): SseEvent[];
        /** End the input: an event the input ended inside is dropped (a CR ends its line at once, so this returns no events). */
        finish(): SseEvent[];
    }
    /** The `hull:sse-client` module object. */
    export interface SseClientModule {
        /** The events of a streamed response; throws the read's Error (its `code`) when the stream ends with one. The stream is closed at the end, on an error and when the loop is left early; concurrent next() calls wait their turn. */
        events(res: HttpStream, opts?: SseOptions): AsyncIterableIterator<SseEvent>;
        /** A fresh parser. */
        parser(opts?: SseOptions): SseParser;
    }
    /** Server-Sent Events client. */
    export const sseClient: SseClientModule;
    /** Server-Sent Events client (default export). */
    const _default: SseClientModule;
    export default _default;
}
