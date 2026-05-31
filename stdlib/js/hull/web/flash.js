/**
 * @file hull:web:flash
 * @module hull:web:flash
 * @description One-shot user notifications across POST/redirect/GET.
 *   Lua parity: `hull.web.flash`.
 *
 * Classic "flash message" pattern: a handler stashes a short string
 * ("Saved.", "Email already exists", ...), the next request reads
 * and clears it. Survives exactly one render. Lives entirely in the
 * session row so it works across redirects.
 *
 * Two emission paths:
 *
 *   1. **Session-backed** (`flash.set` / `flash.consume`). The
 *      classic POST → redirect → GET workflow. Requires
 *      `hull/web/middleware/session` to be wired so that
 *      `req.ctx.session_id` is populated. The flash array lives at
 *      `session.flash`; `consume` drains and persists the cleared
 *      state.
 *
 *   2. **HTMX `HX-Trigger`** (`flash.trigger`). For fragment-swap
 *      paths that don't redirect. Emits
 *      `HX-Trigger: {"flash":{"text":..., "kind":...}}` so the
 *      client-side `htmx:on:flash` listener can render a toast.
 *      No session needed.
 *
 * Composition note: `flash.trigger` sets `HX-Trigger` directly. If
 * the same response also calls `htmx.trigger(...)`, the second call
 * wins. To fire multiple events in one response, use
 * `htmx.trigger(res, { flash: {...}, otherEvent: {...} })` and skip
 * `flash.trigger`.
 *
 * @license AGPL-3.0-or-later
 */

import { json } from "hull:json";
import { session } from "hull:web:middleware:session";

// Bounds the flash queue so a misbehaving handler can't grow
// session.flash unboundedly. The whole queue is JSON-encoded into
// the session row on every write; an unbounded queue balloons
// session storage.
const FLASH_MAX_ENTRIES = 16;
const FLASH_MAX_TEXT_BYTES = 4096;

// Resolve the current session object from req.ctx, hydrating from
// session.load() if the middleware hasn't populated it yet. Returns
// [sessObject, sessionId] or [null, null] if no session.
function getSessionCtx(req) {
    if (!req || !req.ctx || !req.ctx.session_id) return [null, null];
    const sid = req.ctx.session_id;
    let sess = req.ctx.session;
    if (!sess) {
        sess = session.load(sid);
        if (!sess) return [null, null];
        req.ctx.session = sess;
    }
    return [sess, sid];
}

/**
 * Stash a flash message for the next render.
 *
 * Appends `{text, kind}` to the session's flash array and persists.
 * Multiple calls in one request accumulate; the next `flash.consume`
 * returns them in insertion order.
 *
 * @param {object} req   Current request (must have `req.ctx.session_id`).
 * @param {string} text  Message text (coerced to string).
 * @param {string} [kind="info"]  Category tag for styling
 *   (`"success"`, `"error"`, `"info"`, `"warn"`).
 * @throws if no session is wired or `text` is missing.
 */
function set(req, text, kind) {
    if (text == null) {
        throw new Error("flash.set: text is required");
    }
    const [sess, sid] = getSessionCtx(req);
    if (!sess) {
        throw new Error(
            "flash.set: no session in req.ctx.session_id "
            + "(wire hull/web/middleware/session first)"
        );
    }
    if (!sess.flash) sess.flash = [];
    // Silently drop when the queue is at the cap — a handler
    // accumulating without consuming is buggy; growing the session
    // row to multiple KB on every render is worse than dropping.
    if (sess.flash.length >= FLASH_MAX_ENTRIES) return;
    let s = String(text);
    if (s.length > FLASH_MAX_TEXT_BYTES) {
        s = s.slice(0, FLASH_MAX_TEXT_BYTES);
    }
    sess.flash.push({
        text: s,
        kind: kind == null ? "info" : String(kind),
    });
    session.update(sid, sess);
}

/**
 * Drain pending flash messages.
 *
 * Returns the messages array (possibly empty) and clears storage.
 * Idempotent on an empty queue. Safe to call without a session
 * (returns `[]`).
 *
 * @param {object} req   Current request.
 * @returns {Array<{text:string,kind:string}>}
 *
 * @example
 *   const msgs = flash.consume(req);
 *   res.html(template.render("page.html", { flash: msgs, ... }));
 */
function consume(req) {
    const [sess, sid] = getSessionCtx(req);
    if (!sess || !sess.flash || sess.flash.length === 0) return [];
    const msgs = sess.flash;
    sess.flash = [];
    session.update(sid, sess);
    return msgs;
}

/**
 * Fire a client-side flash event via `HX-Trigger`.
 *
 * Sets the `HX-Trigger` response header to
 * `{"flash":{"text":..., "kind":...}}`. Client-side HTMX fires a DOM
 * event named `flash` with the payload; the app listens with
 * `document.body.addEventListener("flash", e => ...)` and renders a
 * toast. Use for HTMX fragment-swap paths where no redirect happens.
 *
 * @param {object} res   Response object.
 * @param {string} text  Message text.
 * @param {string} [kind="info"]  Category tag.
 * @throws on missing `res` or `text`.
 */
function trigger(res, text, kind) {
    if (res == null) {
        throw new Error("flash.trigger: res is required");
    }
    if (text == null) {
        throw new Error("flash.trigger: text is required");
    }
    const payload = json.encode({
        flash: {
            text: String(text),
            kind: kind == null ? "info" : String(kind),
        },
    });
    res.header("HX-Trigger", payload);
}

const flash = { set, consume, trigger };
export { flash };
