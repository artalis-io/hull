// hull:csv - RFC 4180 CSV parser and writer.
declare module "hull:csv" {
    /** Delimiter options shared by parse and encode. */
    export interface CsvDialect {
        /** Field separator: one ASCII character, not CR / LF (default `,`). */
        separator?: string;
        /** Quote character: one ASCII character, not CR / LF, not the separator (default `"`). */
        quote?: string;
    }

    /** Options of `csv.parse`. */
    export interface CsvParseOptions extends CsvDialect {
        /** Treat the first row as a header and return objects. */
        headers?: boolean;
        /** Row limit (default 100000); more throws. */
        maxRows?: number;
        /** Lua-parity alias of `maxRows`. */
        max_rows?: number;
    }

    /** A cell value `csv.encode` writes (null / undefined as empty). */
    export type CsvCell = string | number | boolean | bigint | null | undefined;

    /** Options of `csv.encode`. */
    export interface CsvEncodeOptions extends CsvDialect {
        /** Rows are objects; write a header row. */
        headers?: boolean;
        /** Header order for object rows (default: every row's keys, first seen first). */
        columns?: readonly (string | number)[] | null;
        /** Prefix `'` to cells starting with = + - @ (formula injection defence; default true). */
        sanitizeFormulas?: boolean;
        /** Lua-parity alias of `sanitizeFormulas`. */
        sanitize_formulas?: boolean;
    }

    /** CSV parse and encode. */
    export const csv: {
        /** Parse CSV text: rows of strings, or with `headers: true` one object per data row. Empty input gives `[]`. */
        readonly parse: {
            (text: string, opts: CsvParseOptions & { headers: true }): Record<string, string>[];
            (text: string, opts?: CsvParseOptions | null): string[][];
        };
        /** Encode rows (arrays, or objects with `headers: true`) as CSV text with LF line endings. */
        readonly encode: {
            (rows: readonly { readonly [column: string]: CsvCell }[], opts: CsvEncodeOptions & { headers: true }): string;
            (rows: readonly (readonly CsvCell[])[], opts?: CsvEncodeOptions | null): string;
        };
    };
}
