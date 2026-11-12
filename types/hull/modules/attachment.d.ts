// hull:attachment - content-addressed attachment storage (hull:blob bytes + a metadata row).
declare module "hull:attachment" {
    /** Options for `attachment.init` (sticky: only the keys present are updated). */
    export interface AttachmentInitOptions {
        /** Per-attachment byte cap; null / false = unlimited (the default). */
        maxSize?: number | null | false;
        /** Seconds an unreferenced blob waits before `sweep` may unlink it (default 300). */
        orphanGrace?: number;
        /** Allowed sniffed MIME types; null = any (the default). */
        mimeAllowlist?: readonly string[] | null;
    }
    /** The multipart part `attachment.store` streams (a file part of `req.multipart()`). */
    export interface AttachmentPart {
        /** The uploaded file name (a part without one is refused). */
        readonly filename?: string | null;
        /** The declared Content-Type (recorded, never trusted). */
        readonly contentType?: string | null;
        /** The part's bytes, chunk by chunk. */
        chunks(): AsyncIterable<ArrayBuffer>;
    }
    /** Options for `attachment.store`. */
    export interface AttachmentStoreOptions {
        /** Opaque uploader id recorded on the row. */
        uploadedBy?: string | null;
    }
    /** An attachment's metadata row. */
    export interface AttachmentMetadata {
        /** Attachment id (32 hex characters). */
        id: string;
        /** The blob id (SHA-256 hex). */
        blob_id: string;
        /** The uploaded file name. */
        original_name: string;
        /** The sniffed MIME type. */
        mime: string;
        /** The declared Content-Type. */
        declared_mime: string | null;
        /** Size in bytes. */
        size: number;
        /** Uploader id, if recorded. */
        uploaded_by: string | null;
        /** Unix seconds of the upload. */
        uploaded_at: number;
        /** Reference count. */
        refcount: number;
    }
    /** Options for `attachment.sweep`. */
    export interface AttachmentSweepOptions {
        /** Only blobs queued at least this many seconds ago (default: init's orphanGrace). */
        grace?: number;
        /** Most blobs to unlink (an integer; default 100, at most 10000). */
        limit?: number;
    }
    /** The `hull:attachment` module object (`blob.init` must run first). */
    export interface AttachmentModule {
        /** Create the tables and set the limits. */
        init(opts?: AttachmentInitOptions): void;
        /** Stream a file part into the store; resolves to the new attachment id. */
        store(part: AttachmentPart, opts?: AttachmentStoreOptions): Promise<string>;
        /** The metadata row, or null. */
        metadata(id: string): AttachmentMetadata | null;
        /** The bytes, or null when missing. */
        read(id: string): ArrayBuffer | null;
        /** Write the bytes to `dst` (an fs.write path); bytes written, or null when missing or corrupt. */
        readToFile(id: string, dst: string): number | null;
        /** Unlink unreferenced blobs; returns how many. */
        sweep(opts?: AttachmentSweepOptions): number;
        /** Drop one reference (the row at 0); whether the attachment existed. */
        delete(id: string): boolean;
    }
    /** Attachment storage. */
    export const attachment: AttachmentModule;
}
