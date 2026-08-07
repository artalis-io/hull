/**
 * @file hull:web:htmx:inline-edit
 * @module hull:web:htmx:inline-edit
 * @description HTMX inline-edit widget — click-to-edit single
 * field, pure hypermedia (no client JS for the mode switch).
 *
 * Canonical htmx click-to-edit pattern. The cell starts as a
 * read-only `<span>` that issues GET <editUrl> on click; the
 * server returns the edit-form fragment. The form submits via
 * PATCH <saveUrl> and the server returns the display fragment.
 * Cancel re-fetches the display fragment via GET <cancelUrl>.
 *
 * Two server endpoints per editable field:
 *   GET   editUrl    → return inline_edit.editor(...)
 *   PATCH saveUrl    → validate, persist, return inline_edit.cell(...)
 *   GET   cancelUrl  → return inline_edit.cell(...)
 *
 * A tiny client JS (~15 LOC) at /static/hull/htmx/inline-edit/
 * inline-edit.js handles input focus after the editor swaps in.
 * Esc-to-cancel is wired via hx-trigger on the Cancel button —
 * no JS for that.
 *
 * Lua parity: same surface as `hull.web.htmx.inline-edit`. Names
 * map snake_case (Lua) → camelCase (JS).
 */

// HTML escaping (shared across the htmx widgets).
import { htmx } from "hull:web:htmx";
const esc = htmx.escape;

/**
 * Render the display-mode cell (read-only span that swaps to
 * the editor on click).
 *
 * Required: opts.value, opts.editUrl.
 * Optional: opts.label (default "Edit").
 *
 * role="button" + tabindex="0" make the cell keyboard-
 * activatable; Enter and Space on the focused span fire the
 * click (browser default for role="button"). Swap is outerHTML
 * so the entire cell is replaced by the editor fragment.
 */
function cell(opts) {
    opts = opts || {};
    const value   = esc(opts.value);
    const editUrl = esc(opts.editUrl);
    const label   = esc(opts.label || "Edit");
    return '<span class="hull-inline-edit-view" role="button" tabindex="0"'
        + ` aria-label="${label}" title="${label}"`
        + ` hx-get="${editUrl}"`
        + ` hx-trigger="click, keyup[key=='Enter'] from:this, keyup[key==' '] from:this"`
        + ' hx-swap="outerHTML">'
        + value
        + '</span>';
}

/**
 * Render the edit-mode form (input + Save + Cancel buttons).
 *
 * Required: opts.value, opts.saveUrl, opts.cancelUrl.
 * Optional: opts.name (default "value"), opts.label (default
 * "Edit value"), opts.saveLabel ("Save"), opts.cancelLabel ("Cancel").
 *
 * Submit PATCHes saveUrl; the server response replaces the
 * form. Cancel button (or Esc on the input) GETs cancelUrl
 * for the same swap.
 */
function editor(opts) {
    opts = opts || {};
    const value       = esc(opts.value);
    const saveUrl     = esc(opts.saveUrl);
    const cancelUrl   = esc(opts.cancelUrl);
    const name        = esc(opts.name || "value");
    const label       = esc(opts.label || "Edit value");
    const saveLabel   = esc(opts.saveLabel || "Save");
    const cancelLabel = esc(opts.cancelLabel || "Cancel");
    return '<form class="hull-inline-edit-form"'
        + ` hx-patch="${saveUrl}"`
        + ' hx-target="this" hx-swap="outerHTML">'
        + `<input type="text" name="${name}"`
        + ` value="${value}" aria-label="${label}">`
        + '<button type="submit" class="hull-inline-edit-save">'
        + saveLabel + '</button>'
        + '<button type="button" class="hull-inline-edit-cancel"'
        + ` hx-get="${cancelUrl}"`
        + ` hx-trigger="click, keyup[key=='Escape'] from:closest form"`
        + ' hx-target="closest form" hx-swap="outerHTML">'
        + cancelLabel + '</button>'
        + '</form>';
}

export const inlineEdit = { cell, editor };
export default inlineEdit;
