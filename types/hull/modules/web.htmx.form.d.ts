// hull:web:htmx:form - validation-error rendering for htmx forms.
declare module "hull:web:htmx:form" {
    /** A field-name -> message map (e.g. from `validate.check`); null / undefined render nothing. */
    export type FormErrors = { [field: string]: string } | null | undefined;
    /** The `hull:web:htmx:form` module object. */
    export interface HtmxFormModule {
        /** Whole-form error summary `<ul role="alert">`, or "" when there are no errors. */
        errors(errors: FormErrors): string;
        /** Inline error `<span>` for one field, or "" when it has none. */
        fieldError(errors: FormErrors, name: string): string;
        /** `aria-invalid` + `aria-describedby` attributes for a field with an error, or "". */
        fieldAttrs(errors: FormErrors, name: string): string;
    }
    /** htmx form-error helpers. */
    export const form: HtmxFormModule;
    export default form;
}
