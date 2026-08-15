// spans_huge — THREE repeated windows over a sparse file, each at a distinct
// > 4 GiB offset, plus hull_span_setup() capacity probing, via the spanmeta
// plugin (public SDK). GET /meta?cap=N ->
//   "ret=<true>;filled=<f>;names=big0,big1,big2;foffs=<f0>,<f1>,<f2>;first=..;..."
import { app } from "hull:app";
import { compute } from "hull:compute";
import { fs } from "hull:fs";

app.manifest({
    modules: ["hull/compute@1", "hull/fs@1", "hull/http-server@1"],
    fs: { read: ["huge.bin"] },
});

// 0x100000003, 0x110000005, 0x130000007 — all > 4 GiB, distinct, within 5 GiB.
const OFF = [4294967299, 4563402757, 5100273671];

function dec(buf) {
    const v = new Uint8Array(buf);
    let s = "";
    for (let i = 0; i < v.length; i++) s += String.fromCharCode(v[i]);
    return s;
}

app.get("/meta", (req, res) => {
    const cap = Number.parseInt(req.query.cap, 10) || 16;
    // input byte 0 = requested out_cap; optional query name follows (find over
    // the entries setup actually populated).
    let input = String.fromCharCode(cap % 256);
    if (req.query.find) input += req.query.find;
    const w0 = fs.mmap("huge.bin", { offset: OFF[0], length: 256 });
    const w1 = fs.mmap("huge.bin", { offset: OFF[1], length: 256 });
    const w2 = fs.mmap("huge.bin", { offset: OFF[2], length: 256 });
    let out;
    try {
        out = compute.call("spanmeta", input, {
            spans: [
                { name: "big0", buffer: w0 },
                { name: "big1", buffer: w1 },
                { name: "big2", buffer: w2 },
            ],
        });
    } finally {
        w0.close(); w1.close(); w2.close();
    }
    res.text(dec(out));
});
