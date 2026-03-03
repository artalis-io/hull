// test_i18n.js -- Tests for hull:i18n
//
// Covers: load/locale, t(), number(), date(), currency(), detect(), reset()

import { i18n } from "hull:i18n";

let pass = 0;
let fail = 0;

function test(name, fn) {
    i18n.reset();
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
        throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}

function assertNull(a, msg) {
    if (a !== null)
        throw new Error((msg || "") + " expected null, got " + String(a));
}

// ── Sample locale tables ─────────────────────────────────────────────

const en = {
    format: {
        decimalSep: ".",
        thousandsSep: ",",
        datePattern: "YYYY-MM-DD",
        currency: {
            USD: { symbol: "$", position: "before", decimalDigits: 2 },
            EUR: { symbol: "\u20ac", position: "before", decimalDigits: 2 },
        }
    },
    invoice: {
        title: "Invoice",
        total: "Total: ${amount}",
        status: {
            draft: "Draft",
            sent: "Sent",
        }
    },
    nav: { home: "Home" },
    item: { one: "${count} item", other: "${count} items" },
};

const hu = {
    format: {
        decimalSep: ",",
        thousandsSep: " ",
        datePattern: "YYYY.MM.DD.",
        currency: {
            HUF: { symbol: "Ft", position: "after", decimalDigits: 0 },
            EUR: { symbol: "\u20ac", position: "before", decimalDigits: 2 },
        }
    },
    invoice: {
        title: "Sz\u00e1mla",
        total: "\u00d6sszesen: ${amount} Ft",
    },
    nav: { home: "F\u0151oldal" },
};

// ── load / locale ────────────────────────────────────────────────────

test("load and set locale", () => {
    i18n.load("en", en);
    assertEq(i18n.locale(), null);
    assertEq(i18n.locale("en"), "en");
    assertEq(i18n.locale(), "en");
});

test("switch between locales", () => {
    i18n.load("en", en);
    i18n.load("hu", hu);
    i18n.locale("en");
    assertEq(i18n.t("invoice.title"), "Invoice");
    i18n.locale("hu");
    assertEq(i18n.t("invoice.title"), "Sz\u00e1mla");
});

test("load errors on bad args", () => {
    let threw = false;
    try { i18n.load(42, {}); } catch (e) { threw = true; }
    assertEq(threw, true, "numeric name");

    threw = false;
    try { i18n.load("en", null); } catch (e) { threw = true; }
    assertEq(threw, true, "null table");
});

// ── t() ──────────────────────────────────────────────────────────────

test("simple key lookup", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.t("invoice.title"), "Invoice");
});

test("nested key lookup", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.t("invoice.status.draft"), "Draft");
});

test("interpolation", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.t("invoice.total", { amount: "1,500" }), "Total: 1,500");
});

test("missing key returns key path", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.t("missing.path"), "missing.path");
});

test("missing nested key returns key path", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.t("invoice.nonexistent"), "invoice.nonexistent");
});

test("no active locale returns key path", () => {
    assertEq(i18n.t("anything"), "anything");
});

test("interpolation with no params", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.t("invoice.total"), "Total: ${amount}");
});

test("plural pattern (developer-driven)", () => {
    i18n.load("en", en);
    i18n.locale("en");
    let n = 1;
    assertEq(i18n.t(n === 1 ? "item.one" : "item.other", { count: n }), "1 item");
    n = 5;
    assertEq(i18n.t(n === 1 ? "item.one" : "item.other", { count: n }), "5 items");
});

// ── number() ─────────────────────────────────────────────────────────

test("number with thousands separator", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.number(1500), "1,500");
});

test("number with decimal", () => {
    i18n.load("hu", hu);
    i18n.locale("hu");
    assertEq(i18n.number(1500.5), "1 500,5");
});

test("negative number", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.number(-42000), "-42,000");
});

test("zero", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.number(0), "0");
});

test("small number no grouping", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.number(42), "42");
});

// ── date() ───────────────────────────────────────────────────────────

test("date with known timestamp", () => {
    i18n.load("en", en);
    i18n.locale("en");
    // 2024-01-15 12:00:00 UTC = 1705320000
    assertEq(i18n.date(1705320000), "2024-01-15");
});

test("date with Hungarian pattern", () => {
    i18n.load("hu", hu);
    i18n.locale("hu");
    assertEq(i18n.date(1705320000), "2024.01.15.");
});

test("date epoch zero", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.date(0), "1970-01-01");
});

test("date default pattern without locale format", () => {
    i18n.load("bare", { greeting: "hi" });
    i18n.locale("bare");
    assertEq(i18n.date(1705320000), "2024-01-15");
});

// ── currency() ───────────────────────────────────────────────────────

test("currency HUF (after, 0 digits)", () => {
    i18n.load("hu", hu);
    i18n.locale("hu");
    assertEq(i18n.currency(1500, "HUF"), "1 500 Ft");
});

test("currency EUR (before, 2 digits)", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.currency(1234.5, "EUR"), "\u20ac1,234.50");
});

test("currency USD (before, 2 digits)", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.currency(99.9, "USD"), "$99.90");
});

test("currency unknown code fallback", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.currency(100, "GBP"), "100 GBP");
});

// ── detect() ─────────────────────────────────────────────────────────

test("detect exact match", () => {
    i18n.load("en", en);
    i18n.load("hu", hu);
    assertEq(i18n.detect("hu,en;q=0.9"), "hu");
});

test("detect quality sorting", () => {
    i18n.load("en", en);
    i18n.load("hu", hu);
    assertEq(i18n.detect("en;q=0.5,hu;q=0.9"), "hu");
});

test("detect base language match", () => {
    i18n.load("en", en);
    assertEq(i18n.detect("en-US,de;q=0.9"), "en");
});

test("detect no match returns null", () => {
    i18n.load("en", en);
    assertNull(i18n.detect("ja,zh;q=0.9"));
});

test("detect null returns null", () => {
    assertNull(i18n.detect(null));
});

test("detect request object duck-typing", () => {
    i18n.load("en", en);
    i18n.load("hu", hu);
    const req = {
        header: function(name) {
            if (name === "Accept-Language") return "hu;q=1.0,en;q=0.5";
            return null;
        }
    };
    assertEq(i18n.detect(req), "hu");
});

// ── reset() ──────────────────────────────────────────────────────────

test("reset clears all state", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.t("invoice.title"), "Invoice");
    i18n.reset();
    assertNull(i18n.locale());
    assertEq(i18n.t("invoice.title"), "invoice.title");
});

// ── edge cases ───────────────────────────────────────────────────────

test("deeply nested key", () => {
    i18n.load("en", en);
    i18n.locale("en");
    assertEq(i18n.t("invoice.status.sent"), "Sent");
});

test("non-string value at key returns key", () => {
    i18n.load("en", en);
    i18n.locale("en");
    // "invoice" resolves to an object, not a string
    assertEq(i18n.t("invoice"), "invoice");
});

export default { pass, fail };
