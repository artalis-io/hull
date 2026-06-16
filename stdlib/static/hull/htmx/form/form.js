/*
 * Hull HTMX form widget — client runtime.
 *
 * Adds an automatic loading state to the submit button of any
 * form that triggers an htmx request. While the request is in
 * flight:
 *
 *   - submit button gets `aria-busy="true"` and `disabled`
 *   - if the button has `data-loading-label="Saving..."`, its
 *     text content is swapped to that value (and restored on
 *     completion). The original label is saved in
 *     `data-orig-label` for round-trip safety.
 *
 * Apps style appearance via:
 *
 *   button[aria-busy="true"] {
 *       cursor: wait;
 *       /* optional: opacity, spinner background, etc. */
 *   }
 *
 * Listens at the document level so a single subscription covers
 * every form on every page load. Activated by `htmx:beforeRequest`;
 * cleared by `htmx:afterRequest` (htmx fires this on both success
 * AND error — including `htmx:responseError` — so the button
 * always restores).
 *
 * No DOM cleanup is needed on swap: the submit button lives on
 * the form, not in the swap target.
 *
 * Server-side helpers: `hull.web.htmx.form` (Lua) /
 * `hull:web:htmx:form` (JS) for errors / fieldError / fieldAttrs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
(function () {
    "use strict";

    /**
     * Find the submit button associated with this request.
     *
     * htmx events report `evt.detail.elt` as the triggering
     * element. For a form submit, that's usually the form itself.
     * For an `hx-trigger="click"` button inside or outside a form
     * it's the button. We resolve to the actual submit element:
     *
     *   1. If elt is itself a submit-shaped element, use it.
     *   2. Otherwise, if elt is (or is inside) a form, use the
     *      form's default submit button.
     *   3. Otherwise (button outside a form, link, etc.) — return
     *      null and let the busy state apply to elt itself.
     */
    function findSubmit(elt) {
        if (!elt || !elt.tagName) return null;
        var tag = elt.tagName;
        var type = (elt.type || "").toLowerCase();
        if (tag === "BUTTON" || (tag === "INPUT" && (type === "submit" || type === "button"))) {
            return elt;
        }
        var form = tag === "FORM" ? elt : elt.closest && elt.closest("form");
        if (form) {
            // button:not([type="button"]) covers both <button> and
            // <button type="submit"> (button's default is submit).
            // input[type="submit"] is the separate form-submit input.
            return form.querySelector(
                'button:not([type="button"]), input[type="submit"]'
            );
        }
        return null;
    }

    function setBusy(btn) {
        if (!btn) return;
        // Already in a busy state — don't double-stash the label.
        if (btn.getAttribute("aria-busy") === "true") return;
        btn.setAttribute("aria-busy", "true");
        // disabled prevents double-submit. We track that we set it
        // (vs an already-disabled button) so we know whether to
        // re-enable on completion.
        if (!btn.disabled) {
            btn.disabled = true;
            btn.setAttribute("data-hull-disabled-by-us", "1");
        }
        var label = btn.getAttribute("data-loading-label");
        if (label) {
            btn.setAttribute("data-orig-label", btn.textContent);
            btn.textContent = label;
        }
    }

    function clearBusy(btn) {
        if (!btn) return;
        if (btn.getAttribute("aria-busy") !== "true") return;
        btn.removeAttribute("aria-busy");
        if (btn.getAttribute("data-hull-disabled-by-us") === "1") {
            btn.disabled = false;
            btn.removeAttribute("data-hull-disabled-by-us");
        }
        var orig = btn.getAttribute("data-orig-label");
        if (orig !== null) {
            btn.textContent = orig;
            btn.removeAttribute("data-orig-label");
        }
    }

    function onBeforeRequest(evt) {
        setBusy(findSubmit(evt.detail.elt));
    }
    function onAfterRequest(evt) {
        clearBusy(findSubmit(evt.detail.elt));
    }

    function init() {
        // htmx fires htmx:beforeRequest before sending, then
        // htmx:afterRequest after the response (or error). The
        // latter fires for `htmx:responseError` (4xx/5xx) too, so
        // the button restores even when validation fails.
        document.body.addEventListener("htmx:beforeRequest", onBeforeRequest);
        document.body.addEventListener("htmx:afterRequest",  onAfterRequest);
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", init);
    } else {
        init();
    }
})();
