// hull:crypto:hkdf - HKDF-SHA256 (RFC 5869).
declare module "hull:crypto:hkdf" {
    /** HKDF input bytes: a buffer, or a byte string (one char per byte). */
    export type HkdfBytes = string | ArrayBuffer | ArrayBufferView;

    /** Options of `hkdf.derive`. */
    export interface HkdfDeriveOptions {
        /** Extract salt; absent or empty is 32 zero bytes. */
        salt?: HkdfBytes | null;
        /** The label binding the derived key to its use. */
        info?: HkdfBytes | null;
    }

    /** The HKDF-SHA256 functions. */
    export interface Hkdf {
        /** PRK = HMAC-SHA256(salt, ikm); a null / empty salt is HashLen zeros. */
        readonly extract: (salt: HkdfBytes | null | undefined, ikm: HkdfBytes) => ArrayBuffer;
        /** `length` (1..MAX_LENGTH) bytes of keying material from a PRK of at least 32 bytes and a label. */
        readonly expand: (prk: HkdfBytes, info: HkdfBytes | null | undefined, length: number) => ArrayBuffer;
        /** Extract then expand: `length` bytes derived from `ikm`. */
        readonly derive: (ikm: HkdfBytes, length: number, opts?: HkdfDeriveOptions | null) => ArrayBuffer;
        /** The hash length, 32. */
        readonly HASH_LEN: number;
        /** The largest output length, 255 * 32. */
        readonly MAX_LENGTH: number;
    }

    /** HKDF-SHA256. */
    export const hkdf: Hkdf;
    export default hkdf;
}
