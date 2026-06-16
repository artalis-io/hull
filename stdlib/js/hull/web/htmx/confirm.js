/**
 * @file hull:web:htmx:confirm
 * @module hull:web:htmx:confirm
 * @description HTMX confirm dialog — server-side helper.
 *
 * Replaces htmx's default `window.confirm()` popup with a styled
 * native `<dialog>` element rendered by the shipped client JS at
 * /static/hull/htmx/confirm/confirm.js.
 *
 * One helper: `confirm.attrs(question, opts?)`. Returns a pre-
 * formatted, HTML-attribute-escaped string suitable for splicing
 * into a template (use `{{{ ... }}}` or `| raw` depending on engine).
 * Server has nothing functional to do at request time — the dialog
 * flow is entirely client-side — but apps still declare the module
 * in their manifest for audit visibility.
 *
 * Lua parity: same surface as `hull.web.htmx.confirm`.
 */

// HTML attribute-value escaping. Covers the five characters that
// can break out of a double-quoted attribute value.
const ESC = { "&": "&amp;", '"': "&quot;", "<": "&lt;", ">": "&gt;", "'": "&#39;" };
function attrEscape(s) {
    return String(s == null ? "" : s).replace(/[&"<>']/g, (c) => ESC[c]);
}

/**
 * Render htmx + data attributes for a confirm dialog.
 *
 * Always emits `hx-confirm="<escaped question>"`. The client JS
 * intercepts this and shows the styled dialog instead of the
 * browser's native popup.
 *
 * Optional opts:
 *   - `yes`     string — custom confirm-button label. Default "Confirm".
 *   - `no`      string — custom cancel-button label. Default "Cancel".
 *   - `danger`  bool   — emits `data-confirm-danger="true"` for CSS
 *                        targeting of destructive actions.
 *   - `title`   string — optional heading above the question.
 *
 * All values are HTML-attribute-escaped — caller may pass user-
 * supplied text safely.
 *
 * @param {string} question  The yes/no prompt.
 * @param {object} [opts]    { yes, no, danger, title }
 * @returns {string}         Attribute string for unescaped splice.
 */
function attrs(question, opts) {
    opts = opts || {};
    const parts = [`hx-confirm="${attrEscape(question)}"`];
    if (opts.yes)    parts.push(`data-confirm-yes="${attrEscape(opts.yes)}"`);
    if (opts.no)     parts.push(`data-confirm-no="${attrEscape(opts.no)}"`);
    if (opts.danger) parts.push(`data-confirm-danger="true"`);
    if (opts.title)  parts.push(`data-confirm-title="${attrEscape(opts.title)}"`);
    return parts.join(" ");
}

export const confirm = { attrs };
export default confirm;
