// hull:json - JSON encode / decode.
declare module "hull:json" {
    /** JSON codec. */
    export const json: {
        /** Encode a value (`"null"` for a value JSON cannot represent); throws on a cycle or a BigInt. */
        encode(value: unknown): string;
        /** Decode JSON text; throws a SyntaxError on invalid input. `T` is an unchecked assertion. */
        decode<T = Hull.Data>(text: string): T;
    };
}
