// hull:mime - content MIME sniffing.
declare module "hull:mime" {
    /** MIME sniffing from leading bytes. */
    export const mime: {
        /** The MIME type the bytes' signature names, or null when none matches. */
        readonly sniff: (bytes: string | Hull.Bytes | Hull.Image) => string | null;
    };
}
