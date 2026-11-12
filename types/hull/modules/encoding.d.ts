// hull:encoding - byte <-> text codecs: hex, base64, base32, UTF-8, URL percent-encoding.
declare module "hull:encoding" {
    /** Bytes the codecs take: a byte string (one char per byte, 0-255), an ArrayBuffer, a typed array or a DataView. */
    export type ByteInput = string | ArrayBuffer | ArrayBufferView;

    /** Why a strict decode refused its input. */
    export type DecodeReason = "bad_length" | "invalid_char" | "bad_padding" | "non_canonical";

    /** Options of `base64.encode`. */
    export interface Base64EncodeOptions {
        /** Use the url-safe alphabet (`-_`). */
        url?: boolean;
        /** Pad with `=`; defaults to on for the standard alphabet, off for url-safe. */
        pad?: boolean | null;
    }

    /** Options of `base64.decode` / `base64.why`. */
    export interface Base64DecodeOptions {
        /** Use the url-safe alphabet (which takes no padding). */
        url?: boolean;
        /** Skip whitespace. */
        lenient?: boolean;
    }

    /** Options of `base32.decode` / `base32.why`. */
    export interface Base32DecodeOptions {
        /** Skip whitespace and `=`. */
        lenient?: boolean;
    }

    /** Options of `url.encode`. */
    export interface UrlEncodeOptions {
        /** Further ASCII characters to leave unescaped. */
        keep?: string | null;
    }

    /** Options of `url.decode`. */
    export interface UrlDecodeOptions {
        /** Read `+` as a space (form encoding). */
        form?: boolean;
    }

    /** The codec families. */
    export interface Encoding {
        /** Lowercase hex. */
        readonly hex: {
            /** Two lowercase hex characters per byte. */
            readonly encode: (x: ByteInput) => string;
            /** The bytes (a byte string), either case accepted; null on bad input. */
            readonly decode: (text: string) => string | null;
            /** Why `hex.decode(text)` is null, or null if it is not. */
            readonly why: (text: string) => "bad_length" | "invalid_char" | null;
        };
        /** Base64, standard or url-safe. */
        readonly base64: {
            /** Base64 of `x`. */
            readonly encode: (x: ByteInput, opts?: Base64EncodeOptions | null) => string;
            /** The bytes (a byte string); null on bad or non-canonical input. */
            readonly decode: (text: string, opts?: Base64DecodeOptions | null) => string | null;
            /** Why `base64.decode(text, opts)` is null, or null if it is not. */
            readonly why: (text: string, opts?: Base64DecodeOptions | null) => DecodeReason | null;
        };
        /** RFC 4648 base32 (uppercase, unpadded). */
        readonly base32: {
            /** Base32 of `x`. */
            readonly encode: (x: ByteInput) => string;
            /** The bytes (a byte string), either case; null on bad input. */
            readonly decode: (text: string, opts?: Base32DecodeOptions | null) => string | null;
            /** Why `base32.decode(text, opts)` is null, or null if it is not. */
            readonly why: (text: string, opts?: Base32DecodeOptions | null) => "bad_length" | "invalid_char" | "non_canonical" | null;
        };
        /** Text <-> UTF-8 bytes. */
        readonly utf8: {
            /** The UTF-8 bytes of `text` as a byte string; throws on a lone surrogate. */
            readonly encode: (text: string) => string;
            /** Why `utf8.decode(x)` is null, or null if it is not. */
            readonly why: (x: ByteInput) => "invalid_utf8" | null;
            /** The text of UTF-8 bytes, or null when they are not well-formed UTF-8. */
            readonly decode: (x: ByteInput) => string | null;
        };
        /** RFC 3986 percent-encoding of text. */
        readonly url: {
            /** `text` percent-encoded (UTF-8, upper-case hex). */
            readonly encode: (text: string, opts?: UrlEncodeOptions | null) => string;
            /** `text` decoded; malformed escapes leave it as it is. Never throws on a string. */
            readonly decode: (text: string, opts?: UrlDecodeOptions | null) => string;
        };
        /** Byte strings <-> buffers. */
        readonly bytes: {
            /** A byte string (or any buffer) as a Uint8Array, for the C bindings. */
            readonly toU8: (x: ByteInput) => Uint8Array<ArrayBuffer>;
            /** A buffer as a byte string. */
            readonly fromBuffer: (x: ArrayBuffer | ArrayBufferView) => string;
        };
    }

    /** Byte <-> text codecs. */
    export const encoding: Encoding;
    export default encoding;
}
