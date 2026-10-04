/*
 * Hull HTMX inline-edit widget - client runtime.
 *
 * Single responsibility: focus + text-select the input when an
 * edit form swaps into the DOM. The HTML `autofocus` attribute
 * fires only on initial page load, not on htmx-inserted
 * fragments, so we hook `htmx:afterSwap` and do it ourselves.
 *
 * And keyboard activation: Enter / Space on the display span click
 * it (a role="button" span gets no click from the browser), and
 * Escape inside the edit form clicks its Cancel button. These used
 * to be htmx `keyup[key==...]` trigger filters; under the
 * `csp = "htmx"` preset (allowEval:false) htmx cannot evaluate a
 * filter and treats it as true, so ANY keystroke in the editor
 * fired the cancel request and the edit was lost. The sort widget
 * moved to a script for the same reason.
 *
 * Everything else (mode swap, save, cancel) is pure htmx via the
 * attributes emitted by the server helpers.
 *
 * Server-side helpers: `hull.web.htmx.inline-edit` (Lua) /
 * `hull:web:htmx:inline-edit` (JS) for cell + editor.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
(function () {
    "use strict";

    function focusEditor(elt) {
        if (!elt || !elt.querySelector) return;
        // elt may be the form itself or a parent that contains it.
        var form = elt.classList && elt.classList.contains("hull-inline-edit-form")
            ? elt
            : elt.querySelector(".hull-inline-edit-form");
        if (!form) return;
        var input = form.querySelector('input[type="text"]');
        if (!input) return;
        input.focus();
        // select() so the user can type to replace the existing
        // value without manually clearing it first. Matches the
        // affordance of a typical "click to edit" UI.
        if (typeof input.select === "function") input.select();
    }

    document.addEventListener("keydown", function (evt) {
        var el = evt.target;
        if (!el || !el.classList) return;
        var key = evt.key;
        if (el.classList.contains("hull-inline-edit-view")) {
            if (key !== "Enter" && key !== " " && key !== "Spacebar") return;
            evt.preventDefault();   // Space would scroll the page
            el.click();
            return;
        }
        if (key !== "Escape" && key !== "Esc") return;
        var form = el.closest ? el.closest(".hull-inline-edit-form") : null;
        if (!form) return;
        var cancel = form.querySelector(".hull-inline-edit-cancel");
        if (!cancel) return;
        evt.preventDefault();
        cancel.click();
    });

    document.addEventListener("DOMContentLoaded", function () {
        document.body.addEventListener("htmx:afterSwap", function (evt) {
            focusEditor(evt.target);
        });
    });
})();
