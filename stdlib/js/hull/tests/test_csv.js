// test_csv.js — Tests for hull:csv
//
// Tests RFC 4180 CSV parsing and encoding.

import { csv } from "hull:csv";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        print("FAIL: " + name + ": " + e.message);
    }
}

function assertEq(a, b, msg) {
    if (a !== b)
        throw new Error((msg || "") + " expected " + b + ", got " + a);
}

// ── parse: basic ────────────────────────────────────────────────────

test("parse: simple CSV", () => {
    const rows = csv.parse("a,b,c\n1,2,3\n");
    assertEq(rows.length, 2);
    assertEq(rows[0][0], "a");
    assertEq(rows[0][1], "b");
    assertEq(rows[0][2], "c");
    assertEq(rows[1][0], "1");
    assertEq(rows[1][1], "2");
    assertEq(rows[1][2], "3");
});

test("parse: no trailing newline", () => {
    const rows = csv.parse("a,b\n1,2");
    assertEq(rows.length, 2);
    assertEq(rows[1][0], "1");
    assertEq(rows[1][1], "2");
});

test("parse: empty input", () => {
    const rows = csv.parse("");
    assertEq(rows.length, 0);
});

test("parse: CRLF line endings", () => {
    const rows = csv.parse("a,b\r\n1,2\r\n");
    assertEq(rows.length, 2);
    assertEq(rows[0][0], "a");
    assertEq(rows[1][1], "2");
});

// ── parse: quoted fields ────────────────────────────────────────────

test("parse: quoted fields", () => {
    const rows = csv.parse('"hello","world"\n');
    assertEq(rows.length, 1);
    assertEq(rows[0][0], "hello");
    assertEq(rows[0][1], "world");
});

test("parse: quoted field with comma", () => {
    const rows = csv.parse('"a,b",c\n');
    assertEq(rows.length, 1);
    assertEq(rows[0][0], "a,b");
    assertEq(rows[0][1], "c");
});

test("parse: quoted field with newline", () => {
    const rows = csv.parse('"line1\nline2",b\n');
    assertEq(rows.length, 1);
    assertEq(rows[0][0], "line1\nline2");
    assertEq(rows[0][1], "b");
});

test("parse: escaped quotes", () => {
    const rows = csv.parse('"say ""hello""",b\n');
    assertEq(rows.length, 1);
    assertEq(rows[0][0], 'say "hello"');
    assertEq(rows[0][1], "b");
});

test("parse: empty quoted field", () => {
    const rows = csv.parse('"",b\n');
    assertEq(rows.length, 1);
    assertEq(rows[0][0], "");
    assertEq(rows[0][1], "b");
});

// ── parse: headers mode ─────────────────────────────────────────────

test("parse: headers mode returns objects", () => {
    const rows = csv.parse("name,age\nalice,30\nbob,25\n", { headers: true });
    assertEq(rows.length, 2);
    assertEq(rows[0].name, "alice");
    assertEq(rows[0].age, "30");
    assertEq(rows[1].name, "bob");
    assertEq(rows[1].age, "25");
});

// ── parse: custom separator ─────────────────────────────────────────

test("parse: custom separator", () => {
    const rows = csv.parse("a;b;c\n1;2;3\n", { separator: ";" });
    assertEq(rows.length, 2);
    assertEq(rows[0][1], "b");
    assertEq(rows[1][2], "3");
});

// ── encode: basic ───────────────────────────────────────────────────

test("encode: simple rows", () => {
    const result = csv.encode([["a", "b", "c"], ["1", "2", "3"]]);
    assertEq(result, "a,b,c\n1,2,3\n");
});

test("encode: quotes fields with commas", () => {
    const result = csv.encode([["a,b", "c"]]);
    assertEq(result, '"a,b",c\n');
});

test("encode: escapes quotes", () => {
    const result = csv.encode([['say "hello"', "b"]]);
    assertEq(result, '"say ""hello""",b\n');
});

test("encode: quotes fields with newlines", () => {
    const result = csv.encode([["line1\nline2", "b"]]);
    assertEq(result, '"line1\nline2",b\n');
});

test("encode: empty array", () => {
    const result = csv.encode([]);
    assertEq(result, "");
});

// ── encode: headers mode ────────────────────────────────────────────

test("encode: headers mode from objects", () => {
    const result = csv.encode(
        [{ name: "alice", age: "30" }, { name: "bob", age: "25" }],
        { headers: true }
    );
    assertEq(typeof result, "string");
    const rows = csv.parse(result, { headers: true });
    assertEq(rows.length, 2);
    assertEq(rows[0].name, "alice");
    assertEq(rows[0].age, "30");
});

// ── encode: custom separator ────────────────────────────────────────

test("encode: custom separator", () => {
    const result = csv.encode([["a", "b", "c"]], { separator: ";" });
    assertEq(result, "a;b;c\n");
});

// ── roundtrip ───────────────────────────────────────────────────────

test("roundtrip: parse then encode", () => {
    const input = "name,value\nalice,42\n";
    const rows = csv.parse(input);
    const output = csv.encode(rows);
    assertEq(output, input);
});

test("roundtrip: with quotes", () => {
    const input = '"a,b","c""d"\nplain,value\n';
    const rows = csv.parse(input);
    const output = csv.encode(rows);
    assertEq(output, input);
});

export default { pass, fail };
