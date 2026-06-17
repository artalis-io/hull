/*
 * Hull HTMX confirm widget — client runtime.
 *
 * Intercepts htmx's `htmx:confirm` event and shows a native
 * <dialog> in place of the browser's window.confirm() popup.
 * The original request is paused (event.preventDefault) until
 * the user either confirms (issueRequest is called) or cancels
 * / dismisses (issueRequest is never called → request dropped).
 *
 * Customization via data attributes on the triggering element:
 *
 *   data-confirm-yes="Delete"      // confirm button label
 *   data-confirm-no="Keep"         // cancel button label
 *   data-confirm-danger="true"     // adds [data-danger] to yes button
 *   data-confirm-title="..."       // optional heading
 *
 * Single-instance model: if a second `htmx:confirm` fires while a
 * dialog is open, the first prompt is implicitly dropped (its
 * issueRequest is never called) and the dialog re-renders for the
 * new question. Most-recent click always wins.
 *
 * Dismiss semantics: Escape, backdrop click, and explicit cancel
 * all map to "request dropped." There is no path that issues the
 * request without an affirmative button click.
 *
 * Server-side helper: `hull.web.htmx.confirm` (Lua) /
 * `hull:web:htmx:confirm` (JS) for `confirm.attrs(question, opts)`.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
(function () {
    "use strict";

    var DIALOG_ID  = "hull-confirm";

    // Single-instance state. When a dialog is open, `pending` holds
    // the htmx event whose issueRequest we'd call on confirm. A new
    // event overwrites this — the previous request is implicitly
    // dropped (its issueRequest never fires).
    var pending = null;
    var dialog  = null;

    function getDialog() {
        if (dialog && document.body.contains(dialog)) return dialog;
        dialog = document.getElementById(DIALOG_ID);
        if (dialog) return dialog;

        dialog = document.createElement("dialog");
        dialog.id = DIALOG_ID;
        dialog.setAttribute("aria-labelledby", DIALOG_ID + "-title");
        // STATIC LITERAL ONLY — never interpolate user data here.
        // The only string in this template that varies is the
        // dialog id, which is a compile-time constant. Server-
        // controlled strings (question, custom labels, title) go
        // exclusively through applyLabels()'s textContent path.
        dialog.innerHTML =
            '<h2 id="' + DIALOG_ID + '-title" class="hull-confirm-title"></h2>' +
            '<p class="hull-confirm-message"></p>' +
            '<div class="hull-confirm-actions">' +
                '<button type="button" class="hull-confirm-no"></button>' +
                '<button type="button" class="hull-confirm-yes"></button>' +
            '</div>';
        document.body.appendChild(dialog);
        wireDialogEvents(dialog);
        return dialog;
    }

    function close(d, reason) {
        // Capture & clear `pending` BEFORE we resume — otherwise a
        // synchronous re-entry could lose track of which prompt
        // we're answering.
        var p = pending;
        pending = null;
        try { d.close(reason); } catch (_) { /* already closed */ }
        if (reason !== "confirm" || !p) return;

        // Resume the original request. The obvious choice
        // (p.detail.issueRequest()) is a silent no-op in htmx
        // 2.0.9 for verbs like hx-delete on a standalone <button>
        // — both sync inside the event AND deferred after the
        // dialog closes. We instead use htmx.ajax() to issue a
        // fresh request with the same verb / path / target the
        // button would have used. This bypasses the broken
        // deferred-resume path entirely and works for every
        // verb/element combination.
        if (typeof window.htmx === "undefined" || !window.htmx.ajax) return;
        var verb   = p.detail.verb;     // "get" | "post" | "put" | "patch" | "delete"
        var path   = p.detail.path;     // resolved URL
        var elt    = p.detail.elt;      // triggering element (for hx-include etc.)
        var target = p.detail.target;   // swap target
        if (!verb || !path) return;

        // htmx.ajax() fires its own htmx:confirm event for the new
        // request and re-reads the source element's attributes —
        // including hx-confirm, which would pop our dialog a
        // second time (or htmx's native window.confirm, which
        // auto-rejects in headless). Temporarily strip hx-confirm
        // for the duration of the call so the resumed request
        // sees a clean source. Restore in finally so the button
        // keeps working for future clicks.
        var question = elt && elt.getAttribute("hx-confirm");
        if (question !== null && elt) elt.removeAttribute("hx-confirm");
        try {
            window.htmx.ajax(verb, path, { source: elt, target: target });
        } finally {
            if (question !== null && elt) elt.setAttribute("hx-confirm", question);
        }
    }

    function wireDialogEvents(d) {
        d.querySelector(".hull-confirm-yes").addEventListener("click", function () {
            close(d, "confirm");
        });
        d.querySelector(".hull-confirm-no").addEventListener("click", function () {
            close(d, "cancel");
        });
        // <dialog>'s built-in `cancel` event fires on Escape. Map
        // to our cancel path; preventDefault stops the implicit
        // close so we control the reason.
        d.addEventListener("cancel", function (evt) {
            evt.preventDefault();
            close(d, "cancel");
        });
        // Backdrop click — clicking outside the dialog content.
        // The native <dialog> behavior is that clicks on the
        // ::backdrop pseudo-element bubble as clicks on the dialog
        // element itself.
        d.addEventListener("click", function (evt) {
            if (evt.target === d) close(d, "cancel");
        });
    }

    function applyLabels(d, evt) {
        var trigger = evt.detail.target || evt.target;
        var titleEl = d.querySelector(".hull-confirm-title");
        var msgEl   = d.querySelector(".hull-confirm-message");
        var yesEl   = d.querySelector(".hull-confirm-yes");
        var noEl    = d.querySelector(".hull-confirm-no");

        var title  = trigger.getAttribute("data-confirm-title");
        var yes    = trigger.getAttribute("data-confirm-yes")    || "Confirm";
        var no     = trigger.getAttribute("data-confirm-no")     || "Cancel";
        var danger = trigger.getAttribute("data-confirm-danger") === "true";

        // textContent — never innerHTML — for everything that came
        // from a user-controlled attribute. The Lua/JS helper escapes
        // for attribute context; this is defense in depth for hand-
        // written hx-confirm attrs.
        msgEl.textContent = evt.detail.question || "";
        if (title) {
            titleEl.textContent = title;
            titleEl.hidden = false;
        } else {
            titleEl.textContent = "";
            titleEl.hidden = true;
        }
        yesEl.textContent = yes;
        noEl.textContent  = no;
        if (danger) {
            yesEl.setAttribute("data-danger", "true");
        } else {
            yesEl.removeAttribute("data-danger");
        }
    }

    function handleConfirm(evt) {
        // htmx fires htmx:confirm for EVERY request, not just ones
        // from elements with hx-confirm. Bail out early on requests
        // that haven't opted in — otherwise the dialog would pop up
        // on every search keystroke, every form submit, every
        // hx-trigger fire. The presence of `evt.detail.question`
        // is htmx's signal that hx-confirm was on the triggering
        // element (htmx populates it from the attribute value).
        if (!evt.detail || !evt.detail.question) return;


        // Pause htmx's request pipeline. issueRequest() is the only
        // way to resume; we call it from the confirm-button path.
        evt.preventDefault();

        var d = getDialog();
        // Single-instance: if a previous prompt is still open, its
        // pending event is overwritten here. That event's
        // issueRequest will never be called, so the old request is
        // implicitly dropped.
        pending = evt;
        applyLabels(d, evt);
        if (!d.open) d.showModal();
        // Focus cancel by default — safer default for destructive
        // confirmations. The yes-button text varies; cancel is
        // always the "do nothing" path.
        var noBtn = d.querySelector(".hull-confirm-no");
        if (noBtn) noBtn.focus();
    }

    function init() {
        document.body.addEventListener("htmx:confirm", handleConfirm);
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", init);
    } else {
        init();
    }
})();
