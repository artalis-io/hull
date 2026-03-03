-- Hello World — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Visit: http://localhost:3000/
--
-- Graceful shutdown: Ctrl+C triggers drain mode (finishes in-flight
-- requests within 5s). Second Ctrl+C stops immediately.

local i18n = require("hull.i18n")

app.manifest({})

-- ── i18n setup ─────────────────────────────────────────────────────

i18n.load("en", {
    hello   = "Hello from Hull + Lua!",
    greet   = "Hello, ${name}!",
    health  = "ok",
})

i18n.load("hu", {
    hello   = "Szia, Hull + Lua!",
    greet   = "Szia, ${name}!",
    health  = "ok",
})

i18n.locale("en")  -- default

-- ── Routes ─────────────────────────────────────────────────────────

app.get("/", function(req, res)
    local lang = i18n.detect(req.headers["accept-language"])
    if lang then i18n.locale(lang) end

    db.exec("INSERT INTO visits (path, ts) VALUES (?, ?)", {"/", time.now()})
    res:json({
        message = i18n.t("hello"),
        lang = i18n.locale(),
        time = time.datetime(),
    })
end)

app.get("/visits", function(_req, res)
    local visits = db.query("SELECT * FROM visits ORDER BY id DESC LIMIT 20")
    res:json(visits)
end)

app.get("/health", function(_req, res)
    res:json({ status = "ok", runtime = "lua", uptime = time.clock() })
end)

-- Echo body back as JSON
app.post("/echo", function(req, res)
    res:json({ body = req.body })
end)

-- Greet by route param (auto-detects language from Accept-Language)
app.get("/greet/:name", function(req, res)
    local lang = i18n.detect(req.headers["accept-language"])
    if lang then i18n.locale(lang) end

    res:json({
        greeting = i18n.t("greet", { name = req.params.name }),
        lang = i18n.locale(),
    })
end)

-- Greet with body
app.post("/greet/:name", function(req, res)
    local lang = i18n.detect(req.headers["accept-language"])
    if lang then i18n.locale(lang) end

    res:json({
        greeting = i18n.t("greet", { name = req.params.name }),
        lang = i18n.locale(),
        body = req.body,
    })
end)

log.info("Hello app loaded — routes registered")
