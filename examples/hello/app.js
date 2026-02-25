// Hello World — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Visit: http://localhost:3000/

import { app } from "hull:app";
import { db } from "hull:db";
import { time } from "hull:time";
import { log } from "hull:log";

// Initialize database
db.exec("CREATE TABLE IF NOT EXISTS visits (id INTEGER PRIMARY KEY AUTOINCREMENT, path TEXT, ts INTEGER)");

// Routes
app.get("/", (req, res) => {
    db.exec("INSERT INTO visits (path, ts) VALUES (?, ?)", ["/", time.now()]);
    res.json({ message: "Hello from Hull + QuickJS!", time: time.datetime() });
});

app.get("/visits", (req, res) => {
    const visits = db.query("SELECT * FROM visits ORDER BY id DESC LIMIT 20");
    res.json(visits);
});

app.get("/health", (req, res) => {
    res.json({ status: "ok", runtime: "quickjs", uptime: time.clock() });
});

log.info("Hello app loaded — routes registered");
