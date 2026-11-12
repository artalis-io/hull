// hull:web:middleware:inbox - deduplication of incoming events.
declare module "hull:web:middleware:inbox" {
    /** A message id or source: a string, or an integer (stored as its decimal string); at most 255 characters. */
    export type InboxId = string | number;
    /** Per-message options. */
    export interface InboxMarkOptions {
        /** Record lifetime, whole seconds in [1, 31622400] (default: the module TTL). */
        ttl?: number;
    }
    /** The `inbox` module object. */
    export interface InboxModule {
        /** Create the `_hull_inbox_processed` table (idempotent); `ttl` sets the default lifetime (7 days). */
        init(opts?: InboxMarkOptions): void;
        /** Whether `(source, messageId)` was processed within its TTL. Source defaults to `"default"`. */
        isDuplicate(messageId: InboxId | null | undefined, source?: InboxId | null): boolean;
        /** Record `(source, messageId)` as processed (slides the expiry forward). */
        markProcessed(messageId: InboxId | null | undefined, source?: InboxId | null, opts?: InboxMarkOptions): void;
        /** Atomic check-and-mark: true when already processed, else marks it and returns false. */
        checkAndMark(messageId: InboxId | null | undefined, source?: InboxId | null, opts?: InboxMarkOptions): boolean;
        /** Delete expired records; returns the count deleted. */
        cleanup(): number;
    }
    /** Inbox deduplication. */
    export const inbox: InboxModule;
}
