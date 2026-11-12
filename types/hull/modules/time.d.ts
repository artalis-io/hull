// hull:time - clocks and dates.
declare module "hull:time" {
    /** Time helpers. */
    export const time: {
        /** Unix time in seconds. */
        now(): number;
        /** Unix time in milliseconds. */
        nowMs(): number;
        /** A monotonic clock in milliseconds (for measuring intervals). */
        clock(): number;
        /** Today's UTC date, `"YYYY-MM-DD"`. */
        date(): string;
        /** The current UTC date-time, ISO 8601 (`"YYYY-MM-DDTHH:MM:SSZ"`). */
        datetime(): string;
    };
}
