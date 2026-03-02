-- bench_template — Template rendering performance benchmark endpoints
--
-- Workloads:
--   GET /health  — baseline (JSON, no template)
--   GET /simple  — variable substitution only
--   GET /loop    — 50-item loop + conditionals
--   GET /full    — inheritance + include + loop + filters + conditionals
--   GET /health  — baseline (no template)

local template = require("hull.template")

app.manifest({})

-- Prepare data once at startup to isolate template overhead
local items = {}
for i = 1, 50 do
    items[i] = { id = i, name = "Item " .. i, active = (i % 3 ~= 0) }
end

local year = time.date():sub(1, 4)

-- GET /health — JSON baseline (no template)
app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- GET /simple — minimal template: variable substitution only
app.get("/simple", function(_req, res)
    local html = template.render("pages/simple.html", {
        title   = "Simple Benchmark",
        message = "Hello, World!",
    })
    res:html(html)
end)

-- GET /loop — 50-item loop with conditionals
app.get("/loop", function(_req, res)
    local html = template.render("pages/loop.html", {
        title = "Loop Benchmark",
        count = #items,
        items = items,
    })
    res:html(html)
end)

-- GET /full — inheritance + include + loop + filters + conditionals
app.get("/full", function(_req, res)
    local html = template.render("pages/full.html", {
        title = "Full Benchmark",
        user  = "benchuser",
        year  = year,
        count = #items,
        items = items,
    })
    res:html(html)
end)

log.info("bench_template loaded — endpoints: /health /simple /loop /full")
