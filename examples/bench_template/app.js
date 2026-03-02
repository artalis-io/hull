// bench_template — Template rendering performance benchmark endpoints
//
// Workloads:
//   GET /health  — baseline (JSON, no template)
//   GET /simple  — variable substitution only
//   GET /loop    — 50-item loop + conditionals
//   GET /full    — inheritance + include + loop + filters + conditionals

import { app } from "hull:app";
import { log } from "hull:log";
import { template } from "hull:template";

app.manifest({});

// Prepare data once at startup to isolate template overhead
const items = [];
for (let i = 1; i <= 50; i++) {
    items.push({ id: i, name: `Item ${i}`, active: (i % 3 !== 0) });
}

const year = new Date().getFullYear().toString();

// GET /health — JSON baseline (no template)
app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// GET /simple — minimal template: variable substitution only
app.get("/simple", (_req, res) => {
    const html = template.render("pages/simple.html", {
        title:   "Simple Benchmark",
        message: "Hello, World!",
    });
    res.html(html);
});

// GET /loop — 50-item loop with conditionals
app.get("/loop", (_req, res) => {
    const html = template.render("pages/loop.html", {
        title: "Loop Benchmark",
        count: items.length,
        items: items,
    });
    res.html(html);
});

// GET /full — inheritance + include + loop + filters + conditionals
app.get("/full", (_req, res) => {
    const html = template.render("pages/full.html", {
        title: "Full Benchmark",
        user:  "benchuser",
        year:  year,
        count: items.length,
        items: items,
    });
    res.html(html);
});

log.info("bench_template loaded — endpoints: /health /simple /loop /full");
