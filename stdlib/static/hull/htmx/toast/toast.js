/*
 * Hull HTMX toast widget — client runtime.
 *
 * Listens for `toast` events triggered by the server via the
 * HX-Trigger response header. Each event renders one item into a
 * stack at the bottom-right of the viewport.
 *
 * DOM contract (also targeted by toast.css):
 *
 *   <ol id="hull-toast" aria-live="polite" aria-relevant="additions">
 *     <li class="hull-toast-item" data-level="info" data-toast-id="...">
 *       <span class="hull-toast-message">...</span>
 *       <button type="button" class="hull-toast-dismiss"
 *               aria-label="Dismiss">×</button>
 *     </li>
 *     ...
 *   </ol>
 *
 * The container is auto-created on the first toast if the app
 * didn't put one in the base template. Apps wanting to override
 * placement should declare their own <ol id="hull-toast"> earlier
 * in <body>.
 *
 * Server-side helper: `hull.web.htmx.toast` (Lua) /
 * `hull:web:htmx:toast` (JS).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
(function () {
    "use strict";

    var CONTAINER_ID = "hull-toast";
    var DEFAULT_DURATION_MS = 4000;
    var KNOWN_LEVELS = { info: 1, success: 1, warning: 1, error: 1 };

    // Track in-flight toasts by id so a server that fires the same
    // id twice (e.g., on rapid retries) doesn't stack duplicates.
    var byId = Object.create(null);

    function getContainer() {
        var c = document.getElementById(CONTAINER_ID);
        if (c) return c;
        c = document.createElement("ol");
        c.id = CONTAINER_ID;
        c.setAttribute("aria-live", "polite");
        c.setAttribute("aria-relevant", "additions");
        document.body.appendChild(c);
        return c;
    }

    function removeItem(item) {
        if (!item || !item.parentNode) return;
        var id = item.getAttribute("data-toast-id");
        if (id) delete byId[id];
        item.parentNode.removeChild(item);
    }

    function renderToast(payload) {
        if (!payload || typeof payload !== "object") return;
        var message = payload.message == null ? "" : String(payload.message);
        var level = KNOWN_LEVELS[payload.level] ? payload.level : "info";
        var id = payload.id ? String(payload.id) : null;
        var duration = payload.duration;
        if (typeof duration !== "number" || isNaN(duration)) {
            duration = DEFAULT_DURATION_MS;
        }
        // Error toasts get role="alert" so AT announces them
        // assertively; non-error toasts ride the container's
        // aria-live=polite.
        var role = level === "error" ? "alert" : "status";

        // Dedup by id: if the same id is already on-screen, replace
        // its content and reset its timer instead of adding a peer.
        if (id && byId[id]) {
            var existing = byId[id];
            existing.querySelector(".hull-toast-message").textContent = message;
            existing.setAttribute("data-level", level);
            existing.setAttribute("role", role);
            if (existing._hullTimer) {
                clearTimeout(existing._hullTimer);
                existing._hullTimer = null;
            }
            if (duration > 0) {
                existing._hullTimer = setTimeout(function () {
                    removeItem(existing);
                }, duration);
            }
            return;
        }

        var item = document.createElement("li");
        item.className = "hull-toast-item";
        item.setAttribute("data-level", level);
        item.setAttribute("role", role);
        if (id) item.setAttribute("data-toast-id", id);

        var span = document.createElement("span");
        span.className = "hull-toast-message";
        // textContent — never innerHTML — keeps server-provided
        // strings safe from injection.
        span.textContent = message;

        var btn = document.createElement("button");
        btn.type = "button";
        btn.className = "hull-toast-dismiss";
        btn.setAttribute("aria-label", "Dismiss");
        btn.textContent = "×"; // ×
        btn.addEventListener("click", function () {
            if (item._hullTimer) {
                clearTimeout(item._hullTimer);
                item._hullTimer = null;
            }
            removeItem(item);
        });

        item.appendChild(span);
        item.appendChild(btn);

        if (id) byId[id] = item;
        getContainer().appendChild(item);

        if (duration > 0) {
            item._hullTimer = setTimeout(function () {
                removeItem(item);
            }, duration);
        }
    }

    function handleTrigger(evt) {
        // htmx fires CustomEvent on document body whose `detail`
        // is the JSON value associated with the event name in the
        // HX-Trigger header. Server sends:
        //   HX-Trigger: {"toast": {"message": "...", ...}}
        // → htmx fires evt of type "toast" with detail = the inner
        // object.
        renderToast(evt.detail);
    }

    // htmx fires the event on `body` (the default target). We
    // listen there so a single subscription handles every toast
    // for the page lifetime.
    function init() {
        document.body.addEventListener("toast", handleTrigger);
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", init);
    } else {
        init();
    }
})();
