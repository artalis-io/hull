// test_htmx_form.js. Tests for hull:web:htmx:form helpers
// (errors / fieldError / fieldAttrs).

import { form } from "hull:web:htmx:form";

let pass = 0, fail = 0;
function test(name, fn) {
    try { fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + e.message); }
}
function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}
function assertMatch(s, sub, msg) {
    if (s.indexOf(sub) < 0) throw new Error((msg || "") + " expected '" + sub + "' in '" + s + "'");
}

// ── errors() ────────────────────────────────────────────────────────

test("errors with null returns empty string", () => {
    assertEq(form.errors(null), "");
});

test("errors with empty object returns empty string", () => {
    assertEq(form.errors({}), "");
});

test("errors emits ul.hull-form-errors with role=alert", () => {
    const s = form.errors({ name: "required" });
    assertMatch(s, '<ul class="hull-form-errors" role="alert">');
    assertMatch(s, "</ul>");
});

test("errors li are sorted by field name", () => {
    const s = form.errors({ name: "required", email: "invalid" });
    const iInvalid  = s.indexOf("invalid");
    const iRequired = s.indexOf("required");
    if (iInvalid < 0 || iRequired < 0 || iInvalid > iRequired) {
        throw new Error("expected email error before name error");
    }
});

test("errors li links to inline span via fragment", () => {
    assertMatch(form.errors({ email: "invalid" }), 'href="#hull-form-error-email"');
});

test("errors escapes message body", () => {
    const s = form.errors({ name: "<script>alert(1)</script>" });
    if (s.indexOf("<script>") >= 0) throw new Error("not escaped");
    assertMatch(s, "&lt;script&gt;");
});

// ── fieldError() ────────────────────────────────────────────────────

test("fieldError with null errors returns empty", () => {
    assertEq(form.fieldError(null, "name"), "");
});

test("fieldError for unaffected field returns empty", () => {
    assertEq(form.fieldError({ email: "x" }, "name"), "");
});

test("fieldError emits span with matching id and class", () => {
    assertEq(
        form.fieldError({ name: "required" }, "name"),
        '<span id="hull-form-error-name" class="hull-form-error">required</span>',
    );
});

test("fieldError escapes the message", () => {
    assertMatch(form.fieldError({ name: 'O"Brien' }, "name"), "&quot;Brien");
});

// ── fieldAttrs() ────────────────────────────────────────────────────

test("fieldAttrs with null errors returns empty", () => {
    assertEq(form.fieldAttrs(null, "name"), "");
});

test("fieldAttrs for unaffected field returns empty", () => {
    assertEq(form.fieldAttrs({ email: "x" }, "name"), "");
});

test("fieldAttrs for affected field emits a11y attrs", () => {
    assertEq(
        form.fieldAttrs({ name: "required" }, "name"),
        'aria-invalid="true" aria-describedby="hull-form-error-name"',
    );
});

test("fieldAttrs id matches fieldError id (a11y wiring)", () => {
    const errors = { email: "invalid" };
    const attrs = form.fieldAttrs(errors, "email");
    const span  = form.fieldError(errors, "email");
    assertMatch(attrs, "hull-form-error-email");
    assertMatch(span,  'id="hull-form-error-email"');
});

console.log(`\n${pass}/${pass + fail} htmx-form tests passed`);
if (fail > 0) throw new Error("htmx-form tests failed");
