// hull:image - image decode / encode and pixel buffers.
declare namespace Hull {
    interface Image {
        /** Width in pixels. */
        readonly width: number;
        /** Height in pixels. */
        readonly height: number;
        /** Pixel format. */
        readonly format: "rgba8" | "r8" | "rgba16float" | "r32float";
        /** Pixel data length in bytes. */
        readonly size: number;
        /** A copy of the pixel data. */
        pixels(): ArrayBuffer;
        /** Free the pixels; later property reads throw. Idempotent. */
        close(): void;
    }
}

declare module "hull:image" {
    /** A pixel format. */
    export type ImageFormat = "rgba8" | "r8" | "rgba16float" | "r32float";
    /** A container codec `image.decode` reads. */
    export type ImageCodec = "png" | "jpeg" | "bmp";
    /** Bytes in: a string, an ArrayBuffer, a typed array, a WasmBuffer, a MappedBuffer or an Image's pixels. */
    export type ImageData = string | ArrayBuffer | ArrayBufferView | Hull.WasmBuffer | Hull.MappedBuffer | Hull.Image;

    /** Options of `image.encode`. */
    export interface ImageEncodeOptions {
        /** JPEG quality 1..100 (default 90). */
        quality?: number;
    }

    /** The `hull:image` module object. Failures throw. */
    export interface ImageModule {
        /** An image from a copy of raw pixel data. */
        "new"(width: number, height: number, format: ImageFormat, data: ImageData): Hull.Image;
        /** An image over raw pixel data: borrows a MappedBuffer / WasmBuffer, copies anything else. */
        fromBuffer(data: ImageData, width: number, height: number, format: ImageFormat): Hull.Image;
        /** Decode PNG / JPEG / BMP bytes (codec sniffed unless given). */
        decode(data: ImageData, codec?: ImageCodec | null): Hull.Image;
        /** Encode to PNG or JPEG bytes. */
        encode(img: Hull.Image, codec: "png" | "jpeg", opts?: ImageEncodeOptions): ArrayBuffer;
        /** The image in the compute wire format (u32le width, u32le height, format byte, pixels). */
        toWasm(img: Hull.Image): ArrayBuffer;
        /** An image from the compute wire format. */
        fromWasm(data: ImageData): Hull.Image;
    }
    /** Image codecs. */
    export const image: ImageModule;
}
