-- spans_huge — THREE repeated windows over a sparse file, each at a distinct
-- > 4 GiB offset, plus hull_span_setup() capacity probing, via the spanmeta
-- plugin (public SDK). GET /meta?cap=N ->
--   "ret=<true>;filled=<f>;names=big0,big1,big2;foffs=<f0>,<f1>,<f2>;first=..;..."
-- Each window's DISTINCT 64-bit foffset is reported (proving foffset is not
-- truncated to 32 bits), in declaration order.
local compute = require("hull.compute")
local fs = require("hull.fs")

app.manifest({
    modules = { "hull/compute@1", "hull/fs@1", "hull/http-server@1" },
    fs = { read = { "huge.bin" } },
})

-- 0x100000003, 0x110000005, 0x130000007 — all > 4 GiB, distinct, within 5 GiB.
local OFF = { 4294967299, 4563402757, 5100273671 }

app.get("/meta", function(req, res)
    local cap = tonumber(req.query.cap) or 16
    local w0 = fs.mmap("huge.bin", { offset = OFF[1], length = 256 })
    local w1 = fs.mmap("huge.bin", { offset = OFF[2], length = 256 })
    local w2 = fs.mmap("huge.bin", { offset = OFF[3], length = 256 })
    local out, err = compute.call("spanmeta", string.char(cap % 256), {
        spans = {
            { name = "big0", buffer = w0 },
            { name = "big1", buffer = w1 },
            { name = "big2", buffer = w2 },
        },
    })
    w0:close(); w1:close(); w2:close()
    if err then res:text("ERR " .. tostring(err), 500) else res:text(out) end
end)
