// hull:web:form - application/x-www-form-urlencoded body parsing.
declare module "hull:web:form" {
    /** Options for `form.parse`. */
    export interface FormParseOptions {
        /** Maximum `&`-separated pairs (default 1000); more throws. */
        maxFields?: number;
    }
    /** The `hull:web:form` module object. */
    export interface FormModule {
        /** Parse a URL-encoded body into name -> value (last value wins, null prototype); `{}` for empty input. */
        parse(body: string | null | undefined, opts?: FormParseOptions): { [name: string]: string };
    }
    /** Form-body parser. */
    export const form: FormModule;
}
