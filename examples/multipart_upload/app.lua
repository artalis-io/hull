-- Streaming multipart uploads — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Visit: http://localhost:3000/   (HTML form)
-- Or:    curl -F "user=alice" -F "f=@README.md" http://localhost:3000/upload
--
-- The point of this example is the streaming iterator — bytes pulled
-- out of the socket on demand, never the whole body in memory at
-- once. We hash + size each file part as it streams and return a
-- JSON inventory. Persistent storage is intentionally out of scope:
-- see `docs/roadmap_next.md §1.5.b` for the `hull/attachment@1` module
-- that adds content-addressed disk storage on top of this primitive.

local crypto = require("hull.crypto")
local log    = require("hull.log")

app.manifest({
    name    = "multipart-upload",
    version = "0.0.1",
    modules = {
        "hull/http-server@1",
        "hull/crypto@1",
        "hull/log@1",
    },
})

local FORM_HTML = [[<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>Hull streaming multipart upload</title>
<style>body{font:14px/1.4 system-ui;max-width:40em;margin:2em auto;padding:0 1em}
fieldset{margin:1em 0;padding:1em}label{display:block;margin:.5em 0}
input[type=text],input[type=file]{width:100%;padding:.4em}
button{padding:.5em 1em;font-size:1em}</style>
</head><body>
<h1>Streaming multipart upload</h1>
<form action="/upload" method="post" enctype="multipart/form-data">
  <fieldset>
    <label>Your name <input type="text" name="user" value="alice"></label>
    <label>Tag <input type="text" name="tag" value="demo"></label>
    <label>Files (select one or more) <input type="file" name="files" multiple></label>
    <button>Upload</button>
  </fieldset>
</form>
<p>Or from the shell:</p>
<pre>curl -F "user=alice" -F "f=@README.md" http://localhost:3000/upload</pre>
</body></html>
]]

app.get("/", function(_req, res)
    res:header("Content-Type", "text/html; charset=utf-8")
    res:text(FORM_HTML)
end)

app.get("/health", function(_req, res)
    res:json({ status = "ok", runtime = "lua" })
end)

app.post("/upload", function(req, res)
    local parts = {}

    for part in req:multipart() do
        if part.filename then
            -- File field: stream chunks AND hash incrementally. Bytes
            -- never accumulate — each chunk is fed straight into the
            -- hasher and dropped. Memory use stays O(chunk_size) no
            -- matter how big the upload.
            local hasher = crypto.create_sha256()
            local total = 0
            for chunk in part:chunks() do
                total = total + #chunk
                hasher:update(chunk)
                if total > 5 * 1024 * 1024 then
                    res:status(413)
                    res:json({
                        error       = "file too large",
                        limit_bytes = 5 * 1024 * 1024,
                        name        = part.name,
                        filename    = part.filename,
                    })
                    return
                end
            end
            table.insert(parts, {
                kind         = "file",
                name         = part.name,
                filename     = part.filename,
                content_type = part.content_type,
                size         = total,
                sha256       = hasher:digest(),
            })
        else
            table.insert(parts, {
                kind  = "text",
                name  = part.name,
                value = part:read(),
            })
        end
    end

    log.info(string.format("[upload] %d parts", #parts))
    res:json({ ok = true, parts = parts })
end, {
    multipart = {
        max_part_size  = 5  * 1024 * 1024,
        max_total_size = 16 * 1024 * 1024,
        max_parts      = 32,
    },
})
