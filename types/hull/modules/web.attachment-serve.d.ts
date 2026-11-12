// hull:web:attachment-serve - auth-gated HTTP serving of hull:attachment files.
declare module "hull:web:attachment-serve" {
    /** The attachment metadata row handed to `authCheck`. */
    export interface AttachmentServeMeta {
        /** Attachment id. */
        id: string;
        /** SHA-256 id of the blob holding the bytes. */
        blob_id: string;
        /** Original file name. */
        original_name: string;
        /** Stored (sniffed) MIME type. */
        mime: string;
        /** MIME type the uploader declared. */
        declared_mime: string | null;
        /** Size in bytes. */
        size: number;
        /** Uploader id, if recorded. */
        uploaded_by: string | number | null;
        /** Unix seconds of the upload. */
        uploaded_at: number;
        /** Reference count. */
        refcount: number;
    }
    /** Options for `attachmentServe.serve`. */
    export interface AttachmentServeOptions {
        /** Access check: only `true` (or a Promise of `true`) serves; omitted -> 403, a rejection -> 500. */
        authCheck?: (req: Hull.Request, meta: AttachmentServeMeta) => boolean | Promise<boolean>;
    }
    /** The `hull:web:attachment-serve` module object. */
    export interface AttachmentServeModule {
        /** Serve the attachment (403 / 404 / 304 / 410 / 200); the handler MUST return or await the Promise. */
        serve(req: Hull.Request, res: Hull.Response, id: string, opts?: AttachmentServeOptions): Promise<void>;
    }
    /** Attachment HTTP helper. */
    export const attachmentServe: AttachmentServeModule;
}
