/**
 * @file hull:web:htmx:form
 * @module hull:web:htmx:form
 * @description HTMX form widget — server-side helpers for
 * validation errors and submit-button loading state.
 *
 * Three helpers, all consuming an `errors` object (field-name →
 * message map). Pass `null` / `undefined` to any of them and you
 * get an empty string back, so the same template renders cleanly
 * on the initial GET and after a failed POST.
 *
 *   form.errors(errors)            // whole-form summary block
 *   form.fieldError(errors, name)  // per-field inline span
 *   form.fieldAttrs(errors, name)  // aria-invalid + aria-describedby
 *
 * The submit-button loading state ships as client JS at
 * /static/hull/htmx/form/form.js; no server-side helper is needed.
 *
 * Lua parity: same surface as `hull.web.htmx.form`. Names map
 * snake_case (Lua) → camelCase (JS) per Hull convention.
 */

const BODY_ESC = { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" };
function esc(s) {
    return String(s == null ? "" : s).replace(/[&<>"']/g, (c) => BODY_ESC[c]);
}
function errorId(name) {
    return "hull-form-error-" + esc(name);
}

function hasAny(errors) {
    if (!errors) return false;
    for (const _ in errors) return true;
    return false;
}

/**
 * Render a whole-form error summary as a `<ul role="alert">`.
 * One `<li>` per failing field, each linking to the inline span.
 * Returns "" when errors is null/empty.
 */
function errors(errs) {
    if (!hasAny(errs)) return "";
    const names = Object.keys(errs).sort();
    const parts = ['<ul class="hull-form-errors" role="alert">'];
    for (const name of names) {
        parts.push(`<li><a href="#${errorId(name)}">${esc(errs[name])}</a></li>`);
    }
    parts.push("</ul>");
    return parts.join("");
}

/**
 * Render a single inline error span for one field. The span's
 * `id` is referenced by `fieldAttrs` via `aria-describedby`.
 */
function fieldError(errs, name) {
    if (!errs || !errs[name]) return "";
    return `<span id="${errorId(name)}" class="hull-form-error">${esc(errs[name])}</span>`;
}

/**
 * Render `aria-invalid="true" aria-describedby="..."` for an
 * `<input>` when the named field has an error. Returns "" so
 * inputs don't carry stale aria-invalid on success.
 */
function fieldAttrs(errs, name) {
    if (!errs || !errs[name]) return "";
    return `aria-invalid="true" aria-describedby="${errorId(name)}"`;
}

export const form = { errors, fieldError, fieldAttrs };
export default form;
