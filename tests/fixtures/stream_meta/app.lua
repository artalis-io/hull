-- stream_meta - drive the streamprobe plugin (public hull_stream_* SDK) over a
-- multi-chunk compute.stream and report the per-chunk first/last/index metadata,
-- plus an ordinary compute.call proving non-stream metadata is zero.
local compute = require("hull.compute")

app.manifest({ modules = { "hull/compute@1", "hull/http-server@1" } })

-- GET /stream -> per-chunk "<first>,<last>,<idx>" joined by ";" (host-driven, so
-- identical across runtimes). 768 bytes / chunk 256 = 3 chunks.
app.get("/stream", function(req, res)
    local input = string.rep("x", 768)
    local parts = {}
    compute.stream("streamprobe", input, function(chunk)
        parts[#parts + 1] = string.format("%d,%d,%d",
            string.byte(chunk, 1) or -1, string.byte(chunk, 2) or -1, string.byte(chunk, 3) or -1)
    end, { chunk_size = 256 })
    res:text(table.concat(parts, ";"))
end)

-- GET /nonstream -> "<first>,<last>,<idx>" for an ordinary call (must be 0,0,0)
app.get("/nonstream", function(req, res)
    local out = compute.call("streamprobe", "x")
    res:text(string.format("%d,%d,%d",
        string.byte(out, 1) or -1, string.byte(out, 2) or -1, string.byte(out, 3) or -1))
end)
