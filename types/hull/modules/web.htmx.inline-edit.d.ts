// hull:web:htmx:inline-edit - click-to-edit single field.
declare module "hull:web:htmx:inline-edit" {
    /** Options for `inlineEdit.cell`. */
    export interface InlineEditCellOptions {
        /** The value shown (HTML-escaped). */
        value: unknown;
        /** URL the cell GETs to fetch the editor. */
        editUrl: string;
        /** Accessible label (default "Edit"). */
        label?: string;
    }
    /** Options for `inlineEdit.editor`. */
    export interface InlineEditEditorOptions {
        /** The current value (HTML-escaped). */
        value: unknown;
        /** URL the form PATCHes. */
        saveUrl: string;
        /** URL the Cancel button GETs. */
        cancelUrl: string;
        /** Input name (default "value"). */
        name?: string;
        /** Input aria-label (default "Edit value"). */
        label?: string;
        /** Save-button label (default "Save"). */
        saveLabel?: string;
        /** Cancel-button label (default "Cancel"). */
        cancelLabel?: string;
    }
    /** The `hull:web:htmx:inline-edit` module object. */
    export interface InlineEditModule {
        /** The display-mode `<span>` that swaps to the editor on click. */
        cell(opts: InlineEditCellOptions): string;
        /** The edit-mode `<form>` (input + Save + Cancel). */
        editor(opts: InlineEditEditorOptions): string;
    }
    /** Inline-edit widget helpers. */
    export const inlineEdit: InlineEditModule;
    export default inlineEdit;
}
