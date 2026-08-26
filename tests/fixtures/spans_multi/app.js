// spans_multi - attach THREE named spans over data.bin and ask the spanlist
// plugin (public hull_span.h SDK) to enumerate them in declaration order and
// resolve a caller-supplied name. GET /list?name=<n> ->
//   "count=3;order=alpha,beta,gamma;find=<idx>"  (idx = -1 for an unknown name)
import { app } from "hull:app";
import { compute } from "hull:compute";
import { fs } from "hull:fs";

app.manifest({
    modules: ["hull/compute@1", "hull/fs@1", "hull/http-server@1"],
    fs: { read: ["data.bin"] },
});

function dec(buf) {
    const v = new Uint8Array(buf);
    let s = "";
    for (let i = 0; i < v.length; i++) s += String.fromCharCode(v[i]);
    return s;
}

app.get("/list", (req, res) => {
    const w1 = fs.mmap("data.bin", { offset: 0,    length: 256 });
    const w2 = fs.mmap("data.bin", { offset: 8195, length: 4096 });   // non-page-aligned
    const w3 = fs.mmap("data.bin", { offset: 1000, length: 100 });
    const query = req.query.name || "alpha";
    let out;
    try {
        out = compute.call("spanlist", query, {
            spans: [
                { name: "alpha", buffer: w1 },
                { name: "beta",  buffer: w2 },
                { name: "gamma", buffer: w3 },
            ],
        });
    } finally {
        w1.close(); w2.close(); w3.close();
    }
    res.text(dec(out));
});
