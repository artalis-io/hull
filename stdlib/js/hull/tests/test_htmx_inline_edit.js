// test_htmx_inline_edit.js. Tests for hull:web:htmx:inline-edit.

import { inlineEdit } from "hull:web:htmx:inline-edit";

let pass = 0, fail = 0;
function test(name, fn) {
    try { fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + e.message); }
}
function assertMatch(s, sub, msg) {
    if (s.indexOf(sub) < 0) throw new Error((msg || "") + " expected '" + sub + "' in '" + s + "'");
}
function assertNoMatch(s, sub, msg) {
    if (s.indexOf(sub) >= 0) throw new Error((msg || "") + " did not expect '" + sub + "' in '" + s + "'");
}

// ── cell() ──────────────────────────────────────────────────────────

test("cell emits a span with the value", () => {
    const s = inlineEdit.cell({ value: "Conveyor #3", editUrl: "/x/edit" });
    assertMatch(s, "<span");
    assertMatch(s, "Conveyor #3");
});

test("cell uses outerHTML swap", () => {
    assertMatch(inlineEdit.cell({ value: "x", editUrl: "/x/edit" }), 'hx-swap="outerHTML"');
});

test("cell is keyboard-activatable", () => {
    const s = inlineEdit.cell({ value: "x", editUrl: "/x/edit" });
    assertMatch(s, 'role="button"');
    assertMatch(s, 'tabindex="0"');
});

test("cell triggers on click, Enter, Space", () => {
    const s = inlineEdit.cell({ value: "x", editUrl: "/x/edit" });
    assertMatch(s, "click");
    assertMatch(s, "Enter");
    assertMatch(s, "key==' '");
});

test("cell emits hx-get pointing at editUrl", () => {
    assertMatch(
        inlineEdit.cell({ value: "x", editUrl: "/assets/42/name/edit" }),
        'hx-get="/assets/42/name/edit"',
    );
});

test("cell default label is Edit", () => {
    const s = inlineEdit.cell({ value: "x", editUrl: "/x/edit" });
    assertMatch(s, 'aria-label="Edit"');
    assertMatch(s, 'title="Edit"');
});

test("cell custom label", () => {
    assertMatch(
        inlineEdit.cell({ value: "x", editUrl: "/x/edit", label: "Edit asset name" }),
        'aria-label="Edit asset name"',
    );
});

test("cell escapes value", () => {
    const s = inlineEdit.cell({ value: "<script>alert(1)</script>", editUrl: "/x/edit" });
    assertNoMatch(s, "<script>");
    assertMatch(s, "&lt;script&gt;");
});

test("cell escapes editUrl", () => {
    assertMatch(
        inlineEdit.cell({ value: "x", editUrl: '/x?a="b"' }),
        'hx-get="/x?a=&quot;b&quot;"',
    );
});

// ── editor() ────────────────────────────────────────────────────────

test("editor emits a form with hx-patch", () => {
    const s = inlineEdit.editor({ value: "x", saveUrl: "/x", cancelUrl: "/x/view" });
    assertMatch(s, "<form");
    assertMatch(s, 'hx-patch="/x"');
});

test("editor uses outerHTML swap targeting itself", () => {
    const s = inlineEdit.editor({ value: "x", saveUrl: "/x", cancelUrl: "/x/view" });
    assertMatch(s, 'hx-target="this"');
    assertMatch(s, 'hx-swap="outerHTML"');
});

test("editor input has default name=value", () => {
    assertMatch(
        inlineEdit.editor({ value: "x", saveUrl: "/x", cancelUrl: "/x/view" }),
        'name="value"',
    );
});

test("editor input takes custom name", () => {
    assertMatch(
        inlineEdit.editor({ value: "x", saveUrl: "/x", cancelUrl: "/x/view", name: "title" }),
        'name="title"',
    );
});

test("editor input pre-fills value", () => {
    assertMatch(
        inlineEdit.editor({ value: "Conveyor #3", saveUrl: "/x", cancelUrl: "/x/view" }),
        'value="Conveyor #3"',
    );
});

test("editor default labels are Save / Cancel", () => {
    const s = inlineEdit.editor({ value: "x", saveUrl: "/x", cancelUrl: "/x/view" });
    assertMatch(s, ">Save</button>");
    assertMatch(s, ">Cancel</button>");
});

test("editor custom button labels", () => {
    const s = inlineEdit.editor({
        value: "x", saveUrl: "/x", cancelUrl: "/x/view",
        saveLabel: "Update", cancelLabel: "Keep",
    });
    assertMatch(s, ">Update</button>");
    assertMatch(s, ">Keep</button>");
});

test("editor Cancel button points at cancelUrl", () => {
    assertMatch(
        inlineEdit.editor({ value: "x", saveUrl: "/x", cancelUrl: "/x/view" }),
        'hx-get="/x/view"',
    );
});

test("editor wires Esc-to-cancel", () => {
    assertMatch(
        inlineEdit.editor({ value: "x", saveUrl: "/x", cancelUrl: "/x/view" }),
        "Escape",
    );
});

test("editor escapes value (XSS in pre-fill)", () => {
    const s = inlineEdit.editor({
        value: '"><script>alert(1)</script>',
        saveUrl: "/x", cancelUrl: "/x/view",
    });
    assertNoMatch(s, "<script>");
    assertNoMatch(s, '"><script>');
    assertMatch(s, 'value="&quot;&gt;');
});

test("editor input has aria-label (default)", () => {
    assertMatch(
        inlineEdit.editor({ value: "x", saveUrl: "/x", cancelUrl: "/x/view" }),
        'aria-label="Edit value"',
    );
});

test("editor custom aria-label", () => {
    assertMatch(
        inlineEdit.editor({ value: "x", saveUrl: "/x", cancelUrl: "/x/view", label: "Asset name" }),
        'aria-label="Asset name"',
    );
});

console.log(`\n${pass}/${pass + fail} inline-edit tests passed`);
if (fail > 0) throw new Error("inline-edit tests failed");
