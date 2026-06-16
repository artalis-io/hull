/*
 * Hull HTMX sort widget — client runtime.
 *
 * Adds Enter / Space keyboard activation for sort headers
 * (which use role="button" tabindex="0"). The server-side
 * widget puts those ARIA roles on a <th>, but plain <th>
 * elements don't natively fire `click` on Enter/Space the way
 * a real <button> does — that's a JS responsibility for any
 * role=button on a non-button element.
 *
 * We used to delegate this via an htmx hx-trigger filter like
 * `keyup[key=='Enter'] from:this`. Those bracket-filter
 * expressions go through `new Function()` inside htmx, which
 * is rejected by the `csp = "htmx"` preset (allowEval:false)
 * — and the whole hx-trigger string fails to register when
 * any filter throws, taking the bare `click` listener down
 * with it. Moving keyboard activation here makes the widget
 * work cleanly under strict CSP.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
(function () {
    "use strict";

    function isSortHeader(el) {
        return el && el.classList && el.classList.contains("hull-sort-header");
    }

    function activate(el) {
        // Dispatching a `click` is the cheapest way to reuse the
        // htmx click trigger that's already wired up on the th.
        // No need to re-implement the request path here.
        el.click();
    }

    document.addEventListener("keydown", function (evt) {
        var key = evt.key;
        if (key !== "Enter" && key !== " " && key !== "Spacebar") return;
        var el = evt.target;
        if (!isSortHeader(el)) return;
        // Space scrolls the page by default — that's surprising
        // when the user is interacting with a "button-like" header.
        // Enter is harmless to leave un-prevented but we cancel it
        // too for symmetry.
        evt.preventDefault();
        activate(el);
    });
}());
