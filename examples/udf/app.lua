-- User-Defined SQL Functions — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Demonstrates scalar and aggregate UDFs registered from Lua callbacks.

local db = require("hull.db").default()

app.manifest({
    modules = {
    "hull/http-server@1",
        "hull/db@1",
    },
})

-- ── Lua scalar UDF: uppercase text ────────────────────────────────
db.udf.register("hull_upper", function(text)
    if not text then return nil end
    return string.upper(text)
end, { deterministic = true })

-- ── Lua aggregate UDF: average ────────────────────────────────────
db.udf.register("hull_avg_price", {
    step = function(ctx, val)
        ctx.sum = (ctx.sum or 0) + val
        ctx.count = (ctx.count or 0) + 1
    end,
    finalize = function(ctx)
        if not ctx.count or ctx.count == 0 then return nil end
        return ctx.sum / ctx.count
    end,
}, { aggregate = true })

-- ── Routes ────────────────────────────────────────────────────────

app.get("/health", function(req, res)
    res:json({ ok = true })
end)

-- Scalar UDF: uppercase product names
app.get("/products", function(req, res)
    local rows = db.query("SELECT id, hull_upper(name) as name, category, price FROM products")
    res:json({ products = rows })
end)

-- Aggregate UDF: average price per category
app.get("/avg-prices", function(req, res)
    local rows = db.query("SELECT category, hull_avg_price(price) as avg_price FROM products GROUP BY category")
    res:json({ averages = rows })
end)
