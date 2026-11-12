// hull:web:flash - one-shot notifications across POST / redirect / GET.
declare module "hull:web:flash" {
    /** One flash message. */
    export interface FlashMessage {
        /** Message text (capped at 4096 UTF-8 bytes). */
        text: string;
        /** Category tag (default "info"; capped at 64 bytes). */
        kind: string;
    }
    /** A flash category; any short string is accepted. */
    export type FlashKind = "info" | "success" | "error" | "warn" | (string & {});
    /** The `hull:web:flash` module object. */
    export interface FlashModule {
        /** Queue a message in the session (needs `req.ctx.session_id` from the session middleware); throws without a session or text. */
        set(req: Hull.Request, text: unknown, kind?: FlashKind): void;
        /** Drain the session's queued messages (`[]` without a session). */
        consume(req: Hull.Request): FlashMessage[];
        /** Fire a client-side `flash` event via HX-Trigger (no session needed). */
        trigger(res: Hull.Response, text: unknown, kind?: FlashKind): void;
    }
    /** Flash-message helpers. */
    export const flash: FlashModule;
}
