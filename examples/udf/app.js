// User-Defined SQL Functions — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Demonstrates scalar and aggregate UDFs registered from JS callbacks.

import { app } from "hull:app";
import { db } from "hull:db";

app.manifest({
    modules: [
        "hull/db@1",
    ],
});

// ── JS scalar UDF: uppercase text ─────────────────────────────────
db.udf.register("hull_upper", (text) => {
    if (text === null || text === undefined) return null;
    return String(text).toUpperCase();
}, { deterministic: true });

// ── JS aggregate UDF: average ─────────────────────────────────────
db.udf.register("hull_avg_price", {
    step: (ctx, val) => {
        ctx.sum = (ctx.sum || 0) + val;
        ctx.count = (ctx.count || 0) + 1;
    },
    finalize: (ctx) => {
        if (!ctx.count || ctx.count === 0) return null;
        return ctx.sum / ctx.count;
    },
}, { aggregate: true });

// ── Routes ────────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ ok: true });
});

// Scalar UDF: uppercase product names
app.get("/products", (_req, res) => {
    const rows = db.query("SELECT id, hull_upper(name) as name, category, price FROM products");
    res.json({ products: rows });
});

// Aggregate UDF: average price per category
app.get("/avg-prices", (_req, res) => {
    const rows = db.query("SELECT category, hull_avg_price(price) as avg_price FROM products GROUP BY category");
    res.json({ averages: rows });
});
