// hull:path - lexical path-name manipulation; grants no filesystem authority.
declare module "hull:path" {
    /** Lexical path functions over `/`-separated strings. */
    export const path: {
        /** Collapse `//`, `.` and `..` (never touches disk). */
        readonly normalize: (p: string) => string;
        /** Join components with `/` and normalize; an absolute component resets. */
        readonly join: (...parts: string[]) => string;
        /** The directory portion (`foo` -> `.`, `/foo` -> `/`). */
        readonly dirname: (p: string) => string;
        /** The final component. */
        readonly basename: (p: string) => string;
        /** The final extension including the dot, or `""`. */
        readonly extension: (p: string) => string;
        /** The basename without its final extension. */
        readonly stem: (p: string) => string;
        /** Whether `p` starts with `/`. Not a security check. */
        readonly isAbsolute: (p: string) => boolean;
        /** The lexical relative path from `base` to `target`; throws when incompatible. */
        readonly relative: (base: string, target: string) => string;
        /** Whether `candidate` is lexically `base` or below it. Not a security check. */
        readonly isWithin: (base: string, candidate: string) => boolean;
    };
}
