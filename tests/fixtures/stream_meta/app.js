// stream_meta - drive the streamprobe plugin (public hull_stream_* SDK) over a
// multi-chunk compute.stream and report the per-chunk first/last/index metadata,
// plus an ordinary compute.call proving non-stream metadata is zero.
import { app } from "hull:app";
import { compute } from "hull:compute";

app.manifest({
    modules: ["hull/compute@1", "hull/http-server@1"],
    fs: { read: ["public/"] },
});

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

// File input goes through the fs.read grants, as fs.read does: public/ is
// granted, secret.txt (beside it, in the app directory) is not.
app.get("/streamfile", (req, res) => {
    let n = 0;
    try {
        compute.stream("streamprobe", { file: "public/in.txt" }, () => { n++; },
                       { chunkSize: 256 });
        res.text(`chunks:${n}`);
    } catch (e) {
        res.text(`error:${e.message}`);
    }
});
app.get("/streamdenied", (req, res) => {
    try {
        compute.stream("streamprobe", { file: "secret.txt" }, () => {},
                       { chunkSize: 256 });
        res.text("streamed");
    } catch (e) {
        res.text(`error:${e.message}`);
    }
});
