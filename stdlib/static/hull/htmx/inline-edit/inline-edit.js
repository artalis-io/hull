/*
 * Hull HTMX inline-edit widget — client runtime.
 *
 * Single responsibility: focus + text-select the input when an
 * edit form swaps into the DOM. The HTML `autofocus` attribute
 * fires only on initial page load, not on htmx-inserted
 * fragments, so we hook `htmx:afterSwap` and do it ourselves.
 *
 * Everything else (mode swap, save, cancel, Esc-to-cancel) is
 * pure htmx via the attributes emitted by the server helpers.
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

    document.addEventListener("DOMContentLoaded", function () {
        document.body.addEventListener("htmx:afterSwap", function (evt) {
            focusEditor(evt.target);
        });
    });
})();
