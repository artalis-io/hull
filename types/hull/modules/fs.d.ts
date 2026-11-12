// hull:fs - files under the app directory (or @state/), within manifest `fs` grants.
declare namespace Hull {
    /** A read-only memory-mapped file (`fs.mmap`); usable wherever bytes are taken. */
    interface MappedBuffer {
        /** The mapping's length in bytes (0 once closed). */
        readonly length: number;
        /** Unmap now (otherwise at garbage collection). */
        close(): void;
    }
}

declare module "hull:fs" {
    /** A node's type. */
    export type FsNodeType = "file" | "dir" | "symlink" | "other";

    /** `fs.stat`'s result (lstat semantics: a symlink is not followed). */
    export interface FsStat {
        /** The node's type. */
        type: FsNodeType;
        /** Size in bytes. */
        size: number;
        /** Permission bits. */
        mode: number;
        /** Modification time, Unix seconds. */
        mtime: number;
    }

    /** One entry of `fs.list`. */
    export interface FsEntry {
        /** The entry's name. */
        name: string;
        /** The entry's type. */
        type: FsNodeType;
        /** Size in bytes. */
        size: number;
    }

    /** Options of `fs.write`. */
    export interface FsWriteOptions {
        /** The file's exact permission bits as an octal string within `"666"` (`"600"`). */
        mode?: string;
        /** Shorthand for mode `"600"` (parents it creates are 0700). */
        private?: boolean;
    }

    /** A window of a file for `fs.mmap`. */
    export interface FsMmapWindow {
        /** Byte offset (default 0). */
        offset?: number;
        /** Length in bytes (required, > 0). */
        length: number;
    }

    /** The Error `fs.*` throws, carrying a stable code (`"not_found"`, `"invalid_path"`, `"permission"`, `"file_too_large"`, ...). */
    export interface FsError extends Error {
        /** The failure's code. */
        code: string;
    }

    /** Filesystem access; every call throws an `FsError` on failure. */
    export const fs: {
        /** Read a whole file (refused past the VM heap limit). */
        read(path: string): ArrayBuffer;
        /** Write a file atomically (temp file + rename); returns `true`. */
        write(path: string, data: string | Hull.Bytes, opts?: FsWriteOptions): true;
        /** The node's metadata, or `null` when it does not exist. */
        stat(path: string): FsStat | null;
        /** A directory's entries in byte order. */
        list(path: string): FsEntry[];
        /** Map a file (or a window of it) read-only. */
        mmap(path: string, window?: FsMmapWindow): Hull.MappedBuffer;
    };
}
