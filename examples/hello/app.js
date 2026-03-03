// Hello World — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Visit: http://localhost:3000/
//
// Graceful shutdown: Ctrl+C triggers drain mode (finishes in-flight
// requests within 5s). Second Ctrl+C stops immediately.

import { app } from "hull:app";
import { db } from "hull:db";
import { i18n } from "hull:i18n";
import { log } from "hull:log";
import { time } from "hull:time";

app.manifest({});

// ── i18n setup ─────────────────────────────────────────────────────

i18n.load("en", {
    hello:   "Hello from Hull + QuickJS!",
    greet:   "Hello, ${name}!",
});

i18n.load("hu", {
    hello:   "Szia, Hull + QuickJS!",
    greet:   "Szia, ${name}!",
});

i18n.locale("en");  // default

// ── Routes ─────────────────────────────────────────────────────────

app.get("/", (req, res) => {
    const lang = i18n.detect(req.headers["accept-language"]);
    if (lang) i18n.locale(lang);

    db.exec("INSERT INTO visits (path, ts) VALUES (?, ?)", ["/", time.now()]);
    res.json({
        message: i18n.t("hello"),
        lang: i18n.locale(),
        time: time.datetime(),
    });
});

app.get("/visits", (_req, res) => {
    const visits = db.query("SELECT * FROM visits ORDER BY id DESC LIMIT 20");
    res.json(visits);
});

app.get("/health", (_req, res) => {
    res.json({ status: "ok", runtime: "quickjs", uptime: time.clock() });
});

// Echo body back as JSON
app.post("/echo", (req, res) => {
    res.json({ body: req.body });
});

// Greet by route param (auto-detects language from Accept-Language)
app.get("/greet/:name", (req, res) => {
    const lang = i18n.detect(req.headers["accept-language"]);
    if (lang) i18n.locale(lang);

    res.json({
        greeting: i18n.t("greet", { name: req.params.name }),
        lang: i18n.locale(),
    });
});

// Greet with body
app.post("/greet/:name", (req, res) => {
    const lang = i18n.detect(req.headers["accept-language"]);
    if (lang) i18n.locale(lang);

    res.json({
        greeting: i18n.t("greet", { name: req.params.name }),
        lang: i18n.locale(),
        body: req.body,
    });
});

log.info("Hello app loaded — routes registered");
