-- test_i18n.lua -- Tests for hull.i18n
--
-- Covers: load/locale, t(), number(), date(), currency(), detect(), reset()

local i18n = require('hull.i18n')

local pass = 0
local fail = 0

local function test(name, fn)
    i18n.reset()
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

local function assert_nil(a, msg)
    if a ~= nil then
        error((msg or "") .. " expected nil, got " .. tostring(a))
    end
end

-- ── Sample locale tables ─────────────────────────────────────────────

local en = {
    format = {
        decimal_sep = ".",
        thousands_sep = ",",
        date_pattern = "YYYY-MM-DD",
        currency = {
            USD = { symbol = "$", position = "before", decimal_digits = 2 },
            EUR = { symbol = "\xe2\x82\xac", position = "before", decimal_digits = 2 },
        }
    },
    invoice = {
        title = "Invoice",
        total = "Total: ${amount}",
        status = {
            draft = "Draft",
            sent = "Sent",
        }
    },
    nav = { home = "Home" },
    item = { one = "${count} item", other = "${count} items" },
}

local hu = {
    format = {
        decimal_sep = ",",
        thousands_sep = " ",
        date_pattern = "YYYY.MM.DD.",
        currency = {
            HUF = { symbol = "Ft", position = "after", decimal_digits = 0 },
            EUR = { symbol = "\xe2\x82\xac", position = "before", decimal_digits = 2 },
        }
    },
    invoice = {
        title = "Sz\xc3\xa1mla",
        total = "\xc3\x96sszesen: ${amount} Ft",
    },
    nav = { home = "F\xc5\x91oldal" },
}

-- ── load / locale ────────────────────────────────────────────────────

test("load and set locale", function()
    i18n.load("en", en)
    assert_nil(i18n.locale())
    assert_eq(i18n.locale("en"), "en")
    assert_eq(i18n.locale(), "en")
end)

test("switch between locales", function()
    i18n.load("en", en)
    i18n.load("hu", hu)
    i18n.locale("en")
    assert_eq(i18n.t("invoice.title"), "Invoice")
    i18n.locale("hu")
    assert_eq(i18n.t("invoice.title"), "Sz\xc3\xa1mla")
end)

test("load errors on bad args", function()
    local ok = pcall(i18n.load, 42, {})
    assert_eq(ok, false)
    ok = pcall(i18n.load, "en", "string")
    assert_eq(ok, false)
end)

-- ── t() ──────────────────────────────────────────────────────────────

test("simple key lookup", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.t("invoice.title"), "Invoice")
end)

test("nested key lookup", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.t("invoice.status.draft"), "Draft")
end)

test("interpolation", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.t("invoice.total", {amount = "1,500"}), "Total: 1,500")
end)

test("missing key returns key path", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.t("missing.path"), "missing.path")
end)

test("missing nested key returns key path", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.t("invoice.nonexistent"), "invoice.nonexistent")
end)

test("no active locale returns key path", function()
    assert_eq(i18n.t("anything"), "anything")
end)

test("interpolation with nil params", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.t("invoice.total"), "Total: ${amount}")
end)

test("plural pattern (developer-driven)", function()
    i18n.load("en", en)
    i18n.locale("en")
    local n = 1
    assert_eq(i18n.t(n == 1 and "item.one" or "item.other", {count = n}), "1 item")
    n = 5
    assert_eq(i18n.t(n == 1 and "item.one" or "item.other", {count = n}), "5 items")
end)

-- ── number() ─────────────────────────────────────────────────────────

test("number with thousands separator", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.number(1500), "1,500")
end)

test("number with decimal", function()
    i18n.load("hu", hu)
    i18n.locale("hu")
    assert_eq(i18n.number(1500.5), "1 500,5")
end)

test("negative number", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.number(-42000), "-42,000")
end)

test("zero", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.number(0), "0")
end)

test("small number no grouping", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.number(42), "42")
end)

-- ── date() ───────────────────────────────────────────────────────────

test("date with known timestamp", function()
    i18n.load("en", en)
    i18n.locale("en")
    -- 2024-01-15 12:00:00 UTC = 1705320000
    assert_eq(i18n.date(1705320000), "2024-01-15")
end)

test("date with Hungarian pattern", function()
    i18n.load("hu", hu)
    i18n.locale("hu")
    assert_eq(i18n.date(1705320000), "2024.01.15.")
end)

test("date epoch zero", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.date(0), "1970-01-01")
end)

test("date default pattern without locale format", function()
    i18n.load("bare", { greeting = "hi" })
    i18n.locale("bare")
    assert_eq(i18n.date(1705320000), "2024-01-15")
end)

-- ── currency() ───────────────────────────────────────────────────────

test("currency HUF (after, 0 digits)", function()
    i18n.load("hu", hu)
    i18n.locale("hu")
    assert_eq(i18n.currency(1500, "HUF"), "1 500 Ft")
end)

test("currency EUR (before, 2 digits)", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.currency(1234.5, "EUR"), "\xe2\x82\xac1,234.50")
end)

test("currency USD (before, 2 digits)", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.currency(99.9, "USD"), "$99.90")
end)

test("currency unknown code fallback", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.currency(100, "GBP"), "100 GBP")
end)

-- ── detect() ─────────────────────────────────────────────────────────

test("detect exact match", function()
    i18n.load("en", en)
    i18n.load("hu", hu)
    assert_eq(i18n.detect("hu,en;q=0.9"), "hu")
end)

test("detect quality sorting", function()
    i18n.load("en", en)
    i18n.load("hu", hu)
    assert_eq(i18n.detect("en;q=0.5,hu;q=0.9"), "hu")
end)

test("detect base language match", function()
    i18n.load("en", en)
    assert_eq(i18n.detect("en-US,de;q=0.9"), "en")
end)

test("detect no match returns nil", function()
    i18n.load("en", en)
    assert_nil(i18n.detect("ja,zh;q=0.9"))
end)

test("detect nil returns nil", function()
    assert_nil(i18n.detect(nil))
end)

test("detect request object duck-typing", function()
    i18n.load("en", en)
    i18n.load("hu", hu)
    local req = {
        header = function(self, name)
            if name == "Accept-Language" then return "hu;q=1.0,en;q=0.5" end
            return nil
        end
    }
    assert_eq(i18n.detect(req), "hu")
end)

-- ── reset() ──────────────────────────────────────────────────────────

test("reset clears all state", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.t("invoice.title"), "Invoice")
    i18n.reset()
    assert_nil(i18n.locale())
    assert_eq(i18n.t("invoice.title"), "invoice.title")
end)

-- ── edge cases ───────────────────────────────────────────────────────

test("deeply nested key", function()
    i18n.load("en", en)
    i18n.locale("en")
    assert_eq(i18n.t("invoice.status.sent"), "Sent")
end)

test("non-string value at key returns key", function()
    i18n.load("en", en)
    i18n.locale("en")
    -- "invoice" resolves to a table, not a string
    assert_eq(i18n.t("invoice"), "invoice")
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
