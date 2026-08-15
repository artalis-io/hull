-- spans_multi — attach THREE named spans over data.bin and ask the spanlist
-- plugin (public hull_span.h SDK) to enumerate them in declaration order and
-- resolve a caller-supplied name. GET /list?name=<n> ->
--   "count=3;order=alpha,beta,gamma;find=<idx>"  (idx = -1 for an unknown name)
local compute = require("hull.compute")
local fs = require("hull.fs")

app.manifest({
    modules = { "hull/compute@1", "hull/fs@1", "hull/http-server@1" },
    fs = { read = { "data.bin" } },
})

app.get("/list", function(req, res)
    local w1 = fs.mmap("data.bin", { offset = 0,    length = 256 })
    local w2 = fs.mmap("data.bin", { offset = 8195, length = 4096 })   -- non-page-aligned
    local w3 = fs.mmap("data.bin", { offset = 1000, length = 100 })
    local query = req.query.name or "alpha"
    -- pcall so all mapped buffers are closed even if compute.call raises; a
    -- returned error still flows through `err` exactly as before.
    local ok, out, err = pcall(compute.call, "spanlist", query, {
        spans = {
            { name = "alpha", buffer = w1 },
            { name = "beta",  buffer = w2 },
            { name = "gamma", buffer = w3 },
        },
    })
    w1:close(); w2:close(); w3:close()
    if not ok then res:text("ERR " .. tostring(out), 500)
    elseif err then res:text("ERR " .. tostring(err), 500)
    else res:text(out) end
end)
