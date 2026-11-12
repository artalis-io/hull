// hull:cache - in-process TTL cache (any value) and byte caches over the kv backends.
declare module "hull:cache" {
    import type { KvHandle, KvTtl } from "hull:kv";
    /** Options for `cache.new`. */
    export interface CacheNewOptions {
        /** Entry cap; the least recently used is evicted past it (default 1000). */
        maxEntries?: number;
        /** Seconds used when set / fetch pass no ttl (default: no expiry). */
        defaultTtl?: number | null;
    }

    /** An in-process cache: values stored as-is, process-local, lazily expiring. */
    export interface Cache {
        /** The value, or null on a miss / expiry. */
        get<T = unknown>(key: unknown): T | null;
        /** Whether a live value exists. */
        has(key: unknown): boolean;
        /** Store a value; ttl in seconds (null = no expiry, 0 = expire now, omitted = the default). Returns the value. */
        set<T>(key: unknown, value: T, ttl?: number | null): T;
        /** Get-or-compute: the cached value, else `fn()` stored (a null / undefined result is not stored; a rejected Promise is dropped). */
        fetch<T>(key: unknown, ttl: number | null | undefined, fn: () => T): T;
        /** Get-or-compute with the default ttl. */
        fetch<T>(key: unknown, fn: () => T): T;
        /** Remove a key; whether it existed. */
        delete(key: unknown): boolean;
        /** Remove every entry. */
        clear(): void;
        /** Number of entries (expired ones included until touched). */
        size(): number;
    }

    /** Options for `cache.open`. */
    export interface CacheOpenOptions {
        /** "memory" (default), "sqlite" (over `database`), "valkey" / "redis" (over `dsn`). */
        backend?: "memory" | "sqlite" | "valkey" | "redis";
        /** Logical namespace (default "default"). */
        namespace?: string;
        /** SQL backend: the connection to store in. */
        database?: Hull.DbConnection;
        /** Valkey / Redis DSN. */
        dsn?: string;
        /** Alias of `dsn`. */
        url?: string;
        /** Expiry for writes that pass no ttl, in seconds. */
        defaultTtl?: KvTtl;
        /** Item cap (default 10000 when neither cap is given). */
        maxItems?: number;
        /** Memory backend: byte cap (default 16 MiB when neither cap is given). */
        maxBytes?: number;
    }

    /** A byte cache handle (a kv handle with eviction) plus `fetch`. Values are byte strings. */
    export interface CacheHandle extends KvHandle {
        /** Get-or-compute: the cached bytes, else `fn()` (a byte string) stored and returned. */
        fetch(key: string, ttl: KvTtl, fn: () => string): string;
        /** Get-or-compute with the default ttl. */
        fetch(key: string, fn: () => string): string;
    }

    /** The `hull:cache` module object: the default instance's methods plus `new` / `open`. */
    export interface CacheModule extends Cache {
        /** An isolated in-process cache instance. */
        "new"(opts?: CacheNewOptions): Cache;
        /** Open a byte cache over a kv backend (`encrypt` is refused: use hull:kv). */
        open(opts: CacheOpenOptions): CacheHandle;
    }
    /** In-process cache. */
    export const cache: CacheModule;
    /** In-process cache (default export). */
    const _default: CacheModule;
    export default _default;
}
