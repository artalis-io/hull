// hull:kv - portable key/value store (memory, SQL over a db connection, Valkey / Redis).
declare module "hull:kv" {
    /** A ttl in seconds (0..100 years); `false` = no expiry overriding a default; null / undefined = the default. */
    export type KvTtl = number | false | null | undefined;

    /** A key held in C (`crypto.keyFromEnv`): it seals and opens with its bytes itself. */
    export interface KvKeyHandle {
        /** Seal with the held key. */
        secretbox(...args: never[]): unknown;
        /** Open with the held key. */
        secretboxOpen(...args: never[]): unknown;
    }

    /** Value encryption for `kv.open` (docs/kv_encryption_design.md). */
    export interface KvEncryptOptions {
        /** Key id (an integer 0..2^32-1, or its canonical decimal text) -> 32-byte key (a byte string) or a `crypto.keyFromEnv` key. */
        keys: { [id: string]: string | KvKeyHandle; [id: number]: string | KvKeyHandle };
        /** The id of the key new values are sealed with. */
        current: number | string;
        /** Migration only: accept a plaintext value where a sealed one is expected. */
        allowPlaintext?: boolean;
    }

    /** Options for `kv.open`. */
    export interface KvOpenOptions {
        /** The store: "memory" (default, process-local), "sqlite" / "postgres" (over `database`), "valkey" / "redis" (over `dsn`, needs the valkey feature). */
        backend?: "memory" | "sqlite" | "postgres" | "valkey" | "redis";
        /** Logical namespace (default "default"). */
        namespace?: string;
        /** SQL backends: the connection to store in (e.g. `db.default()`). */
        database?: Hull.DbConnection;
        /** Valkey / Redis: the DSN (`valkey://...` / `redis://...`). */
        dsn?: string;
        /** Valkey / Redis: alias of `dsn`. */
        url?: string;
        /** Expiry for writes that pass no ttl, in seconds. */
        defaultTtl?: KvTtl;
        /** Memory backend: byte cap. */
        maxBytes?: number;
        /** Memory backend: item cap. */
        maxItems?: number;
        /** Seal values with a keyring (authenticated encryption). */
        encrypt?: KvEncryptOptions | null;
    }

    /** What a backend supports; an optional op on a backend without it throws `unsupported`. */
    export interface KvCaps {
        /** Expiry. */
        ttl: boolean;
        /** `incr`. */
        atomic_increment: boolean;
        /** `cas`. */
        compare_exchange: boolean;
        /** `scan`. */
        scan: boolean;
        /** Values survive a restart. */
        persistent: boolean;
        /** Shared across processes. */
        shared: boolean;
        /** Evicts under a cap (caches). */
        eviction: boolean;
        /** Joins the connection's transactions. */
        transactions: boolean;
    }

    /** Backend statistics: the memory store reports counters, a SQL store `items`, Valkey its identity. */
    export interface KvStats {
        /** Memory: reads that found a value. */
        hits?: number;
        /** Memory: reads that found none. */
        misses?: number;
        /** Memory: values evicted under a cap. */
        evictions?: number;
        /** Memory: values dropped on expiry. */
        expirations?: number;
        /** Memory / SQL: live values. */
        items?: number;
        /** Memory: bytes held. */
        bytes?: number;
        /** Valkey: the backend name. */
        backend?: string;
        /** Valkey: the namespace. */
        namespace?: string;
        /** Valkey: always true. */
        shared?: boolean;
    }

    /** Per-write options. */
    export interface KvWriteOptions {
        /** Expiry in seconds. */
        ttl?: KvTtl;
    }

    /** A store handle. Keys and values are byte strings (each char a code unit 0-255). Errors are thrown with a `code` (`invalid_argument`, `unsupported`, `decrypt_failed`, ...). */
    export interface KvHandle {
        /** The logical namespace. */
        readonly namespace: string;
        /** The backend's name ("memory", "sqlite", "postgres", "valkey", ...). */
        readonly backend: string;
        /** What the backend supports. */
        readonly caps: KvCaps;
        /** The value, or null on a miss. */
        get(key: string): string | null;
        /** Store a value. */
        set(key: string, value: string, opts?: KvWriteOptions): true;
        /** Remove a key; whether it existed. */
        delete(key: string): boolean;
        /** Whether a live value exists. */
        exists(key: string): boolean;
        /** Atomically add `by` (default 1) to an integer value; returns the new value. */
        incr(key: string, by?: number, opts?: KvWriteOptions): number;
        /** Atomic compare-and-swap; `expected` null = set only if absent. Whether it swapped. */
        cas(key: string, expected: string | null | undefined, value: string, opts?: KvWriteOptions): boolean;
        /** Encrypted handles: re-seal values under `prefix` not sealed with the current key; returns how many. */
        rekey(prefix?: string, opts?: KvWriteOptions): number;
        /** Remove every value in the namespace. */
        clear(): true;
        /** Keys starting with `prefix`. */
        scan(prefix?: string | null, opts?: { limit?: number }): string[];
        /** Drop expired values; returns how many. */
        cleanup(): number;
        /** Backend statistics. */
        stats(): KvStats;
        /** Release an owned network connection (no-op for memory / SQL). */
        close(): void;
    }

    /** The `hull:kv` module object. */
    export interface KvModule {
        /** Open a store handle. */
        open(opts: KvOpenOptions): KvHandle;
        /** API version ("1"). */
        readonly _VERSION: string;
    }
    /** Key/value store. */
    export const kv: KvModule;
    /** Key/value store (default export). */
    const _default: KvModule;
    export default _default;
}
