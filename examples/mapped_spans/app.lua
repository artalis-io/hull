-- mapped_spans — reference app for Hull mapped spans (checkpoint 3).
--
-- Maps a NON-page-aligned window of data.bin (file offset 8195, length 4096),
-- attaches it read-only as span "source", and asks the spanreader WASM plugin
-- (which uses the public hull_span.h SDK) to resolve a caller-supplied name and
-- sample the window at an optional offset.
--
--   GET /read?name=<span>[&off=<n>]
local compute = require("hull.compute")
local fs = require("hull.fs")

app.manifest({
    modules = { "hull/compute@1", "hull/fs@1", "hull/http-server@1" },
    fs = { read = { "data.bin" } },
})

local function u32le(n)
    n = n % 0x100000000
    return string.char(n % 256,
                       math.floor(n / 256) % 256,
                       math.floor(n / 65536) % 256,
                       math.floor(n / 16777216) % 256)
end

app.get("/read", function(req, res)
    local w = fs.mmap("data.bin", { offset = 8195, length = 4096 })  -- non-page-aligned
    local name = req.query.name or "source"
    local input = name
    if req.query.off then
        input = name .. "\0" .. u32le(tonumber(req.query.off) or 0)
    end
    -- pcall so the mapped buffer is closed even if compute.call raises; a
    -- returned error still flows through `err` exactly as before.
    local ok, out, err = pcall(compute.call, "spanreader", input, {
        spans = { { name = "source", buffer = w } },
    })
    w:close()
    if not ok then res:text("ERR " .. tostring(out), 500)
    elseif err then res:text("ERR " .. tostring(err), 500)
    else res:text(out) end
end)
