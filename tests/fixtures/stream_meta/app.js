// stream_meta — drive the streamprobe plugin (public hull_stream_* SDK) over a
// multi-chunk compute.stream and report the per-chunk first/last/index metadata,
// plus an ordinary compute.call proving non-stream metadata is zero.
import { app } from "hull:app";
import { compute } from "hull:compute";

app.manifest({ modules: ["hull/compute@1", "hull/http-server@1"] });

// GET /stream -> per-chunk "<first>,<last>,<idx>" joined by ";" (host-driven, so
// identical to the Lua app). 768 bytes / chunk 256 = 3 chunks.
app.get("/stream", (req, res) => {
    const input = "x".repeat(768);
    const parts = [];
    compute.stream("streamprobe", input, (chunk) => {
        const v = new Uint8Array(chunk);
        parts.push(`${v[0]},${v[1]},${v[2]}`);
    }, { chunkSize: 256 });
    res.text(parts.join(";"));
});

// GET /nonstream -> "<first>,<last>,<idx>" for an ordinary call (must be 0,0,0)
app.get("/nonstream", (req, res) => {
    const out = new Uint8Array(compute.call("streamprobe", "x"));
    res.text(`${out[0]},${out[1]},${out[2]}`);
});
