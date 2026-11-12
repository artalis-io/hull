// hull:logx - contextual logging: bound fields appended to every line in logfmt.
declare module "hull:logx" {
    /** Fields to bind: each value is written as its string form. */
    export type LogxFields = { [key: string]: unknown };
    /** A logger carrying bound fields. */
    export interface LogxLogger {
        /** Log at INFO (the message is converted to a string; null / undefined is empty). */
        info(msg?: unknown): void;
        /** Log at WARN. */
        warn(msg?: unknown): void;
        /** Log at ERROR. */
        error(msg?: unknown): void;
        /** Log at DEBUG. */
        debug(msg?: unknown): void;
        /** A child logger with more fields. */
        with(fields?: LogxFields | null): LogxLogger;
    }
    /** The `hull:logx` module object. */
    export interface LogxModule extends LogxLogger {
        /** A leading-space logfmt fragment of `fields` ("" for none). */
        fields(fields?: LogxFields | null): string;
    }
    /** Contextual logging. */
    export const logx: LogxModule;
    /** Contextual logging (default export). */
    const _default: LogxModule;
    export default _default;
}
