// hull:web:htmx:confirm - styled confirm dialog for hx-confirm.
declare module "hull:web:htmx:confirm" {
    /** Options for `confirm.attrs`. */
    export interface ConfirmOptions {
        /** Confirm-button label (default "Confirm"). */
        yes?: string;
        /** Cancel-button label (default "Cancel"). */
        no?: string;
        /** Mark the action destructive (`data-confirm-danger="true"`). */
        danger?: boolean;
        /** Heading above the question. */
        title?: string;
    }
    /** The `hull:web:htmx:confirm` module object. */
    export interface ConfirmModule {
        /** Attribute string (`hx-confirm` + data attributes, escaped) to splice into an element unescaped. */
        attrs(question: string, opts?: ConfirmOptions): string;
    }
    /** Confirm-dialog helper. */
    export const confirm: ConfirmModule;
    export default confirm;
}
