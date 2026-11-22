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
        /** The `retry` field (ms) of this event, or null. */
        retry: number | null;
    }
    /** The parser alone, for text from any source. */
    export interface SseParser {
        /** Feed text; returns the events it completes. */
        feed(text: string): SseEvent[];
        /** End the input; returns the events a trailing CR completed (an event the input ended inside is dropped). */
        finish(): SseEvent[];
    }
    /** The `hull:sse-client` module object. */
    export interface SseClientModule {
        /** The events of a streamed response; throws the read's Error (its `code`) when the stream ends with one. Leaving the loop early closes the stream. */
        events(res: HttpStream): AsyncGenerator<SseEvent, void, undefined>;
        /** A fresh parser. */
        parser(): SseParser;
    }
    /** Server-Sent Events client. */
    export const sseClient: SseClientModule;
    /** Server-Sent Events client (default export). */
    const _default: SseClientModule;
    export default _default;
}
