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

    var DIALOG_ID = "hull-confirm";

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
        // Capture & clear `pending` BEFORE issueRequest fires —
        // otherwise the confirm could re-enter via a synchronous
        // htmx:confirm and we'd lose track.
        var p = pending;
        pending = null;
        try { d.close(reason); } catch (_) { /* already closed */ }
        if (reason === "confirm" && p && typeof p.detail.issueRequest === "function") {
            p.detail.issueRequest();
        }
        // For reason === "cancel" or any other value: do nothing.
        // The original request stays paused and is effectively dropped.
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
