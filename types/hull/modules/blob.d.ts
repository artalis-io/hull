// hull:blob - content-addressed blob storage (SHA-256 ids) under an fs.write directory.
declare module "hull:blob" {
    /** Options for `blob.init`. */
    export interface BlobInitOptions {
        /** Store root, inside a `manifest.fs.write` path. */
        dir: string;
        /** Directory sharding levels (default 1). */
        shardDepth?: number;
        /** Seconds before an abandoned temp file is swept. */
        tmpMaxAge?: number;
    }
    /** What a write returns. */
    export interface BlobPutResult {
        /** The blob id: 64 lowercase hex characters (SHA-256 of the bytes). */
        id: string;
        /** Size in bytes. */
        size: number;
    }
    /** Options for `blob.put` / `blob.putVerified`. */
    export interface BlobPutOptions {
        /** fsync the file and its directory before returning. */
        durable?: boolean;
    }
    /** Options for `blob.writer`. */
    export interface BlobWriterOptions extends BlobPutOptions {
        /** The id the bytes must hash to; `finalize` throws otherwise. */
        expected?: string | null;
    }
    /** Options for reads. */
    export interface BlobReadOptions {
        /** Update the blob's access time (default true). */
        trackAccess?: boolean;
    }
    /** A streaming writer. */
    export interface BlobWriter {
        /** Append bytes (a buffer, or a string as UTF-8); returns the writer. */
        write(chunk: Hull.Bytes | string): BlobWriter;
        /** Commit the blob. */
        finalize(): BlobPutResult;
        /** Discard the written bytes (idempotent). */
        abort(): void;
    }
    /** A streaming reader. */
    export interface BlobReader {
        /** Up to `n` bytes (default 65536), or null at the end. */
        read(n?: number): ArrayBuffer | null;
        /** Release the reader. */
        close(): void;
    }
    /** Options for `blob.cleanup`. */
    export interface BlobCleanupOptions {
        /** Evict until the store is at most this many bytes. */
        maxTotalSize?: number;
        /** Evict blobs older than this many seconds. */
        maxAge?: number;
        /** Eviction order (default "lru"). */
        strategy?: "lru" | "fifo";
        /** Report without deleting. */
        dryRun?: boolean;
    }
    /** What `blob.cleanup` returns. */
    export interface BlobCleanupResult {
        /** Blobs removed (or that would be, on a dry run). */
        removed: number;
        /** Bytes freed. */
        freedBytes: number;
    }
    /** The `hull:blob` module object. Every call but `init` throws until `init` ran. */
    export interface BlobModule {
        /** Open (or switch) the store. */
        init(opts: BlobInitOptions): void;
        /** Store bytes (a string is stored as UTF-8). */
        put(bytes: Hull.Bytes | string, opts?: BlobPutOptions): BlobPutResult;
        /** Store bytes that must hash to `sha` (throws on a mismatch). */
        putVerified(bytes: Hull.Bytes | string, sha: string, opts?: BlobPutOptions): BlobPutResult;
        /** A streaming writer. */
        writer(opts?: BlobWriterOptions): BlobWriter;
        /** The bytes, or null when missing. */
        get(id: string, opts?: BlobReadOptions): ArrayBuffer | null;
        /** A streaming reader (throws when missing). */
        reader(id: string, opts?: BlobReadOptions): BlobReader;
        /** Whether the blob exists. */
        exists(id: string): boolean;
        /** Size in bytes, or null when missing. */
        size(id: string): number | null;
        /** Last access time (unix seconds), or null when missing. */
        atime(id: string): number | null;
        /** Remove a blob; whether it existed. */
        delete(id: string): boolean;
        /** A snapshot of every blob. */
        iter(): BlobPutResult[];
        /** Total bytes stored. */
        totalSize(): number;
        /** Number of blobs. */
        count(): number;
        /** Evict blobs by size / age. */
        cleanup(opts: BlobCleanupOptions): BlobCleanupResult;
    }
    /** Content-addressed blob store. */
    export const blob: BlobModule;
}
