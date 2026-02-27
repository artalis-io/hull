-- Hello World — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Visit: http://localhost:3000/

-- Initialize database
db.exec("CREATE TABLE IF NOT EXISTS visits (id INTEGER PRIMARY KEY AUTOINCREMENT, path TEXT, ts INTEGER)")

-- Routes
app.get("/", function(_req, res)
    db.exec("INSERT INTO visits (path, ts) VALUES (?, ?)", {"/", time.now()})
    res:json({ message = "Hello from Hull + Lua!", time = time.datetime() })
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

-- Greet by route param
app.get("/greet/:name", function(req, res)
    res:json({ greeting = "Hello, " .. req.params.name .. "!" })
end)

-- Greet with body
app.post("/greet/:name", function(req, res)
    res:json({ greeting = "Hello, " .. req.params.name .. "!", body = req.body })
end)

log.info("Hello app loaded — routes registered")
