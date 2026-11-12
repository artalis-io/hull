// hull:qrcode - QR Code generator (ISO/IEC 18004), byte mode.
declare module "hull:qrcode" {
    /** Error-correction level: L 7%, M 15%, Q 25%, H 30%. */
    export type QrEcLevel = "L" | "M" | "Q" | "H";

    /** Options of `qrcode.encode`. */
    export interface QrEncodeOptions {
        /** Error-correction level (default `"M"`). */
        ecLevel?: QrEcLevel;
        /** Force mask pattern 0..7; omitted picks the best-scoring one. */
        mask?: number | null;
    }

    /** Options of `qrcode.svg`. */
    export interface QrSvgOptions extends QrEncodeOptions {
        /** Pixels per module, an integer 1-64 (default 4). */
        scale?: number;
        /** Quiet-zone modules, an integer 0-64 (default 4). */
        margin?: number;
        /** Dark color: hex, a color name, rgb()/rgba() (default `#000`). */
        dark?: string;
        /** Light color, or `"none"` for no background (default `#fff`). */
        light?: string;
    }

    /** An encoded QR code. */
    export interface QrCode {
        /** Modules, 1-INDEXED (`matrix[1][1]` is the top-left): 1 dark, 0 light. */
        matrix: (0 | 1)[][];
        /** Modules per side. */
        size: number;
        /** QR version, 1-40. */
        version: number;
        /** The error-correction level used. */
        ecLevel: QrEcLevel;
        /** The mask pattern used, 0..7. */
        mask: number;
    }

    /** QR code generation. */
    export const qrcode: {
        /** Encode `text` (as UTF-8) into a module matrix. */
        readonly encode: (text: string, opts?: QrEncodeOptions | null) => QrCode;
        /** Encode `text` as an SVG document string. */
        readonly svg: (text: string, opts?: QrSvgOptions | null) => string;
    };
}
