// test_confirm.js. Tests for hull:web:htmx:confirm.attrs.

import { confirm } from "hull:web:htmx:confirm";

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

test("attrs with bare question emits only hx-confirm", () => {
    assertEq(confirm.attrs("Delete this?"), 'hx-confirm="Delete this?"');
});

test("attrs escapes double-quote in question", () => {
    assertEq(confirm.attrs('Say "yes"?'), 'hx-confirm="Say &quot;yes&quot;?"');
});

test("attrs escapes ampersand in question", () => {
    assertEq(confirm.attrs("Tom & Jerry?"), 'hx-confirm="Tom &amp; Jerry?"');
});

test("attrs escapes angle brackets (defense in depth)", () => {
    assertMatch(confirm.attrs("Delete <script>?"), 'hx-confirm="Delete &lt;script&gt;?"');
});

test("attrs with yes opt emits data-confirm-yes", () => {
    assertMatch(confirm.attrs("Delete?", { yes: "Delete" }), 'data-confirm-yes="Delete"');
});

test("attrs with no opt emits data-confirm-no", () => {
    assertMatch(confirm.attrs("Delete?", { no: "Keep" }), 'data-confirm-no="Keep"');
});

test("attrs with danger=true emits data-confirm-danger", () => {
    assertMatch(confirm.attrs("Delete?", { danger: true }), 'data-confirm-danger="true"');
});

test("attrs with danger=false omits data-confirm-danger", () => {
    const s = confirm.attrs("Delete?", { danger: false });
    if (s.indexOf("data-confirm-danger") >= 0) {
        throw new Error("data-confirm-danger should not be present when danger=false");
    }
});

test("attrs with title opt emits data-confirm-title", () => {
    assertMatch(
        confirm.attrs("Are you sure?", { title: "Permanent action" }),
        'data-confirm-title="Permanent action"',
    );
});

test("attrs with all opts emits all attributes", () => {
    const s = confirm.attrs("Delete this asset?", {
        yes: "Delete", no: "Keep", danger: true, title: "Cannot undo",
    });
    assertMatch(s, 'hx-confirm="Delete this asset?"');
    assertMatch(s, 'data-confirm-yes="Delete"');
    assertMatch(s, 'data-confirm-no="Keep"');
    assertMatch(s, 'data-confirm-danger="true"');
    assertMatch(s, 'data-confirm-title="Cannot undo"');
});

test("attrs escapes custom labels too", () => {
    assertMatch(
        confirm.attrs("OK?", { yes: 'Delete "all"' }),
        'data-confirm-yes="Delete &quot;all&quot;"',
    );
});

test("attrs with null question yields empty hx-confirm", () => {
    assertEq(confirm.attrs(null), 'hx-confirm=""');
});

console.log(`\n${pass}/${pass + fail} confirm tests passed`);
if (fail > 0) throw new Error("confirm tests failed");
