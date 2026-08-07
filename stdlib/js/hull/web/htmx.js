/**
 * @file hull:web:htmx
 * @module hull:web:htmx
 * @description HTMX request inspection and response-header helpers.
 *
 * Lua parity: same surface as `hull.web.htmx` with camelCase function
 * names (`pushUrl` vs `push_url`, `currentUrl` vs `current_url`, etc.).
 * All other semantics identical.
 *
 * Provides the small server-side surface for the HTMX hypermedia
 * pattern: detect whether an inbound request came from htmx (vs a
 * normal browser navigation), then set the response headers HTMX
 * knows how to interpret (HX-Redirect, HX-Retarget, HX-Reswap,
 * HX-Trigger, etc.). Pure functions; no state, no I/O.
 *
 * Canonical use:
 *
 *     import { htmx } from "hull:web:htmx";
 *     import { template } from "hull:template";
 *
 *     if (htmx.is(req)) {
 *         res.html(template.render("partials/todo_row.html", data));
 *     } else {
 *         res.html(template.render("pages/todos.html", data));
 *     }
 *
 * @license AGPL-3.0-or-later
 */

// ── Shared HTML escape ────────────────────────────────────────────────
//
// The htmx widgets render user data into element text and double-quoted
// attributes, so they all need HTML escaping. It lives here, once, rather
// than being re-rolled in each widget. Escapes the five HTML metacharacters
// plus the backtick (unquoted-attribute contexts), matching hull.template.
const _escapeMap = {
    "&": "&amp;", "<": "&lt;", ">": "&gt;",
    '"': "&quot;", "'": "&#39;", "`": "&#96;",
};

/**
 * HTML-escape a value for safe interpolation into element text or a
 * double-quoted attribute. null/undefined become the empty string.
 */
function escape(s) {
    if (s === null || s === undefined) return "";
    return String(s).replace(/[&<>"'`]/g, (ch) => _escapeMap[ch]);
}

/** True if the request was sent by the htmx client runtime. */
function is(req) {
    if (!req || !req.headers) return false;
    return req.headers["hx-request"] === "true";
}

/** True if the request was sent as an htmx boost (via `hx-boost`). */
function boosted(req) {
    if (!req || !req.headers) return false;
    return req.headers["hx-boosted"] === "true";
}

/** Return the source page URL of an htmx-driven request, or null. */
function currentUrl(req) {
    if (!req || !req.headers) return null;
    return req.headers["hx-current-url"] || null;
}

/** Return the id of the htmx target element, if specified. */
function target(req) {
    if (!req || !req.headers) return null;
    return req.headers["hx-target"] || null;
}

/** Return the name of the htmx trigger element, if specified. */
function triggerName(req) {
    if (!req || !req.headers) return null;
    return req.headers["hx-trigger-name"] || null;
}

/**
 * Send a client-side redirect.
 *
 * For htmx requests, sets HX-Redirect (htmx navigates the browser).
 * For plain requests, falls back to a normal HTTP 302.
 */
function redirect(req, res, path) {
    if (is(req)) {
        res.header("HX-Redirect", path);
        res.status(204);
        res.send("");
    } else {
        res.redirect(path);
    }
}

/**
 * Override the target of an htmx swap.
 *
 * Setting HX-Retarget tells the htmx client to apply the response
 * to a different element than the one specified by `hx-target`.
 */
function retarget(res, selector) {
    res.header("HX-Retarget", selector);
}

/**
 * Override the swap mode of an htmx swap. Accepts the same modes as
 * `hx-swap`: innerHTML, outerHTML, beforebegin, afterbegin, beforeend,
 * afterend, delete, none.
 */
function reswap(res, mode) {
    res.header("HX-Reswap", mode);
}

/**
 * Trigger one or more client-side events.
 *
 * `eventOrTable` can be a bare event name string or an object
 * mapping event names to payloads. If a bare string is given with a
 * non-null `payload`, the payload object is constructed automatically.
 *
 * Optional `opts.timing` selects when the event fires:
 *   undefined -> HX-Trigger (immediate, default)
 *   "swap"    -> HX-Trigger-After-Swap
 *   "settle"  -> HX-Trigger-After-Settle
 *
 * When `eventOrTable` is an object (multi-event form), opts may be
 * passed as the 3rd arg: trigger(res, {...}, { timing: "swap" }).
 */
function trigger(res, eventOrTable, payload, opts) {
    if (typeof eventOrTable === "object" && eventOrTable !== null
        && typeof payload === "object" && payload !== null
        && opts === undefined && payload.timing !== undefined) {
        opts = payload;
        payload = undefined;
    }

    let header = "HX-Trigger";
    if (opts && opts.timing === "swap") header = "HX-Trigger-After-Swap";
    else if (opts && opts.timing === "settle") header = "HX-Trigger-After-Settle";

    let value;
    if (typeof eventOrTable === "object" && eventOrTable !== null) {
        value = JSON.stringify(eventOrTable);
    } else if (payload !== undefined && payload !== null) {
        value = JSON.stringify({ [eventOrTable]: payload });
    } else {
        value = eventOrTable;
    }
    res.header(header, value);
}

/** Trigger a full client-side page refresh (HX-Refresh: true). */
function refresh(res) {
    res.header("HX-Refresh", "true");
}

/**
 * Push a new URL into the browser history.
 *
 * Pass `false` to suppress a default push that htmx would otherwise do.
 */
function pushUrl(res, url) {
    res.header("HX-Push-Url", url === false ? "false" : url);
}

/**
 * Replace the current URL in the browser history (HX-Replace-Url).
 *
 * Pass `false` to suppress.
 */
function replaceUrl(res, url) {
    res.header("HX-Replace-Url", url === false ? "false" : url);
}

/**
 * Issue a client-side navigation without a hard reload (HX-Location).
 *
 * `pathOrOpts` is either a plain path string or an htmx LocationContext
 * object `{ path: "/x", target: "#main", swap: "outerHTML" }`.
 */
function location(res, pathOrOpts) {
    const value = (typeof pathOrOpts === "object" && pathOrOpts !== null)
        ? JSON.stringify(pathOrOpts)
        : pathOrOpts;
    res.header("HX-Location", value);
}

export const htmx = {
    escape,
    is, boosted, currentUrl, target, triggerName,
    redirect, retarget, reswap,
    trigger,
    refresh, pushUrl, replaceUrl, location,
};

export default htmx;
