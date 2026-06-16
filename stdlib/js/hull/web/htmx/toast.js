/**
 * @file hull:web:htmx:toast
 * @module hull:web:htmx:toast
 * @description HTMX toast widget — server-side helper.
 *
 * Sends transient flash messages to the client via the standard
 * HX-Trigger header. The shipped client JS at
 * /static/hull/htmx/toast/toast.js listens for the `toast` event
 * and renders each message into a stack at the bottom of the page.
 *
 * The widget is structural-only by design: layout + a11y CSS but
 * no colors, borders, shadows, animations, or fonts. Apps style
 * per-level appearance by targeting the `data-level` attribute on
 * each toast item.
 *
 * Setup:
 *
 *     // 1. Declare the module in your manifest:
 *     app.manifest({
 *         modules: ["hull/web/htmx@1", "hull/web/htmx/toast@1"],
 *         csp: "htmx",  // so the shipped /static/ JS can load
 *     });
 *
 *     // 2. Include the assets in your base template:
 *     <link rel="stylesheet" href="/static/hull/htmx/toast/toast.css">
 *     <script src="/static/hull/htmx/toast/toast.js" defer></script>
 *
 *     // 3. Fire toasts from any handler:
 *     import { toast } from "hull:web:htmx:toast";
 *     toast.success(res, "Asset saved");
 *     toast.error(res, "Permission denied");
 *
 * Lua parity: identical surface as `hull.web.htmx.toast`.
 */

import { htmx } from "hull:web:htmx";

// Valid level values. Anything else collapses to "info" so the
// client can rely on a known set when styling by [data-level].
const LEVELS = new Set(["info", "success", "warning", "error"]);

/**
 * Send a toast to the client.
 *
 * The HX-Trigger header is set to a JSON object the client JS
 * consumes:
 *
 *     {"toast": {"message": "...", "level": "info", "duration": 4000}}
 *
 * If `htmx.trigger` was already used on this response, the headers
 * compose at the htmx-client level (each top-level key becomes its
 * own event).
 *
 * @param {object} res     Keel response object.
 * @param {string} message Toast text. Plain string; the client
 *                         renders it via `textContent` (HTML safe).
 * @param {object} [opts]
 * @param {string} [opts.level]    "info" (default), "success",
 *                                 "warning", or "error". Unknown
 *                                 values collapse to "info".
 * @param {number} [opts.duration] Auto-dismiss in ms (default 4000).
 *                                 0 means "no auto-dismiss".
 * @param {string} [opts.id]       Optional stable id for the toast
 *                                 (enables client-side dedup).
 */
function show(res, message, opts) {
    opts = opts || {};
    const level = LEVELS.has(opts.level) ? opts.level : "info";
    const payload = {
        message: String(message == null ? "" : message),
        level,
        duration: opts.duration,
        id: opts.id,
    };
    htmx.trigger(res, "toast", payload);
}

/** Shorthand: `toast.show(res, msg, { level: "info" })`. */
function info(res, message, opts) {
    show(res, message, { ...(opts || {}), level: "info" });
}

/** Shorthand: `toast.show(res, msg, { level: "success" })`. */
function success(res, message, opts) {
    show(res, message, { ...(opts || {}), level: "success" });
}

/** Shorthand: `toast.show(res, msg, { level: "warning" })`. */
function warning(res, message, opts) {
    show(res, message, { ...(opts || {}), level: "warning" });
}

/** Shorthand: `toast.show(res, msg, { level: "error" })`. */
function error(res, message, opts) {
    show(res, message, { ...(opts || {}), level: "error" });
}

export const toast = { show, info, success, warning, error };
export default toast;
