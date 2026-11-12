// hull:archive:tar - ustar archives: parse / create, and extract / pack through the fs capability.
declare module "hull:archive:tar" {
    /** Archive or member bytes: a buffer, or a string taken as its UTF-8 text. */
    export type TarBytes = string | Hull.Bytes | Hull.Image;

    /** A member of a parsed archive. */
    export interface TarEntry {
        /** Member name (a directory without its trailing `/`). */
        name: string;
        /** Member contents (empty for a directory). */
        data: ArrayBuffer;
        /** Size in bytes. */
        size: number;
        /** Permission bits. */
        mode: number;
        /** Whether the member is a directory. */
        isDir: boolean;
    }

    /** A member for `tar.create`. */
    export interface TarCreateEntry {
        /** Member name: relative, no `..`, fits the 100-byte ustar field. */
        name: string;
        /** Contents (ignored for a directory; absent is empty). */
        data?: TarBytes | null;
        /** Permission bits (default 0644). */
        mode?: number;
        /** A directory member. */
        isDir?: boolean;
    }

    /** A file for `tar.pack`: an app path, or `{ path, name? }` to store it under another name. */
    export type TarPackFile = string | { path: string; name?: string };

    /** ustar archives. */
    export const tar: {
        /** The members of an archive; throws on a malformed or unsafe one. */
        readonly parse: (bytes: TarBytes) => TarEntry[];
        /** Build an archive (at most 65536 entries). */
        readonly create: (entries: readonly TarCreateEntry[]) => ArrayBuffer;
        /** Write each member under `dir` through the fs capability (needs `manifest.fs.write`); throws on error. */
        readonly extract: (bytes: TarBytes, dir?: string | null) => true;
        /** Read each file through the fs capability (needs `manifest.fs.read`) into an archive. */
        readonly pack: (files: readonly TarPackFile[]) => ArrayBuffer;
    };
}
