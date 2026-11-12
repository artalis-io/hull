// hull:i18n - message lookup with ${var} interpolation, number / date / currency formatting.
declare module "hull:i18n" {
    /** A currency's formatting in a locale. */
    export interface I18nCurrency {
        /** The symbol (default: the ISO code). */
        symbol?: string;
        /** Minor-unit digits (default 2). */
        decimalDigits?: number;
        /** Lua-parity alias of `decimalDigits`. */
        decimal_digits?: number;
        /** Where the symbol goes (default before). */
        position?: "before" | "after";
    }

    /** A locale's number / date / currency formatting (`format` in its table). */
    export interface I18nFormat {
        /** Decimal separator (default `.`). */
        decimalSep?: string;
        /** Lua-parity alias of `decimalSep`. */
        decimal_sep?: string;
        /** Thousands separator (default `,`). */
        thousandsSep?: string;
        /** Lua-parity alias of `thousandsSep`. */
        thousands_sep?: string;
        /** Date pattern with YYYY MM DD HH mm ss tokens (default `YYYY-MM-DD`). */
        datePattern?: string;
        /** Lua-parity alias of `datePattern`. */
        date_pattern?: string;
        /** ISO 4217 code -> its formatting. */
        currency?: { [code: string]: I18nCurrency };
    }

    /** Nested message keys -> strings with `${var}` placeholders. */
    export interface I18nMessages {
        /** A message, or a nested group reached by a dotted key. */
        [key: string]: string | I18nMessages;
    }

    /** A locale table: messages, plus an optional `format`. */
    export interface I18nLocaleTable {
        /** Number / date / currency formatting. */
        format?: I18nFormat;
        /** A message, or a nested group. */
        [key: string]: string | I18nMessages | I18nFormat | undefined;
    }

    /** Values interpolated into `${name}` placeholders (missing ones stay as written). */
    export type I18nParams = { [name: string]: string | number | boolean | bigint | null | undefined };

    /** An object with a `header(name)` method, such as a request. */
    export interface I18nHeaderSource {
        /** The value of header `name`. */
        header(name: string): unknown;
    }

    /** Internationalization (process-global state). */
    export const i18n: {
        /** Register a locale table under `name`. */
        readonly load: (name: string, table: I18nLocaleTable) => void;
        /** Set the active locale when given; returns the active locale (process-global: prefer the *In forms across an await). */
        readonly locale: (name?: string | null) => string | null;
        /** Translate in the active locale; the key itself when missing. */
        readonly t: (key: string, params?: I18nParams | null) => string;
        /** Translate in an explicit locale (safe across an await). */
        readonly tIn: (locale: string | null | undefined, key: string, params?: I18nParams | null) => string;
        /** Format a number with the active locale's separators. */
        readonly number: (n: number) => string;
        /** Format a number with locale `locale`'s separators. */
        readonly numberIn: (locale: string | null | undefined, n: number) => string;
        /** Format a unix timestamp (seconds, UTC) with the active locale's date pattern. */
        readonly date: (timestamp: number) => string;
        /** Format a unix timestamp with locale `locale`'s date pattern. */
        readonly dateIn: (locale: string | null | undefined, timestamp: number) => string;
        /** Format an amount in currency `code` (ISO 4217) per the active locale. */
        readonly currency: (amount: number, code: string) => string;
        /** Format an amount in currency `code` per locale `locale`. */
        readonly currencyIn: (locale: string | null | undefined, amount: number, code: string) => string;
        /** The best loaded locale for an Accept-Language value (or a request), or null. */
        readonly detect: (headerOrReq: string | I18nHeaderSource | null | undefined) => string | null;
        /** Drop every locale and the active one. */
        readonly reset: () => void;
    };
}
