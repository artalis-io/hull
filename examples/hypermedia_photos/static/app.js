// hypermedia_photos - tiny client-side helpers.
//
// All inline `hx-on:` attributes need either 'unsafe-eval' or
// 'unsafe-inline' in script-src because htmx compiles them via the
// Function constructor. Instead of relaxing CSP, attach real event
// listeners here (this file ships with the page nonce).

// Reset the new-entry form after a successful submit so the input
// clears for the next entry. The form's hx-target is #entry-feed
// (innerHTML), which destroys + replaces the form's surroundings but
// keeps the form itself; we just need to wipe its inputs.
document.addEventListener("htmx:afterRequest", (e) => {
    const form = e.detail?.elt;
    if (form && form.id === "new-entry" && e.detail.successful) {
        form.reset();
    }
});
