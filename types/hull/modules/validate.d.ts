// hull:validate - schema-based input validation.
declare module "hull:validate" {
    /** A custom validator's verdict: true / null / undefined valid, false invalid, a string the error message. */
    export type ValidateVerdict = boolean | string | null | undefined | void;

    /** The rules for one field (all optional). */
    export interface ValidateRule {
        /** The field must be present and not `""`. */
        required?: boolean;
        /** Trim a string value (written back into the data). */
        trim?: boolean;
        /** The value's type; an unknown one throws. */
        type?: "string" | "number" | "integer" | "boolean";
        /** Minimum string length (code points) or numeric value. */
        min?: number;
        /** Maximum string length (code points) or numeric value. */
        max?: number;
        /** A regular expression SOURCE (<= 1024 chars; a RegExp object throws). */
        pattern?: string;
        /** The allowed values (compared with ===). */
        oneof?: readonly unknown[];
        /** The value must be an email address. */
        email?: boolean;
        /** A custom validator. */
        fn?: (value: unknown, field: string, data: Record<string, unknown>) => ValidateVerdict;
        /** The error message used for any failed rule of this field. */
        message?: string;
    }

    /** Field name -> rules. */
    export type ValidateSchema = { [field: string]: ValidateRule };

    /** Schema validation. */
    export const validate: {
        /** `[true, null]`, or `[false, errors]` mapping each failed field to its message. Non-object data counts as `{}`. */
        readonly check: (data: unknown, schema: ValidateSchema) =>
            [ok: true, errors: null] | [ok: false, errors: Record<string, string>];
    };
}
