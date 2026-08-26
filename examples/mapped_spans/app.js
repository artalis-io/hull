// mapped_spans - reference app for Hull mapped spans (checkpoint 3).
// Mirrors app.lua: maps a non-page-aligned window of data.bin, attaches it as
// span "source", and calls the spanreader plugin (public hull_span.h SDK).
//   GET /read?name=<span>[&off=<n>]
import { app } from "hull:app";
import { compute } from "hull:compute";
import { fs } from "hull:fs";

app.manifest({
    modules: ["hull/compute@1", "hull/fs@1", "hull/http-server@1"],
    fs: { read: ["data.bin"] },
});

const dec = (b) => { const u = new Uint8Array(b); let s = ""; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };

// Build a binary-safe input: name bytes, optional NUL + u32le offset. (Using an
// ArrayBuffer avoids UTF-8 mangling of the high bytes a JS string would incur.)
function buildInput(name, off) {
    const bytes = [];
    for (let i = 0; i < name.length; i++) bytes.push(name.charCodeAt(i) & 0xff);
    if (off !== null && off !== undefined) {
        const n = off >>> 0;
        bytes.push(0, n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff);
    }
    return new Uint8Array(bytes).buffer;
}

app.get("/read", (req, res) => {
    const w = fs.mmap("data.bin", { offset: 8195, length: 4096 });  // non-page-aligned
    const name = req.query.name || "source";
    const off = (req.query.off !== undefined) ? (Number.parseInt(req.query.off, 10) || 0) : null;
    let out;
    try {
        out = compute.call("spanreader", buildInput(name, off), {
            spans: [{ name: "source", buffer: w }],
        });
    } finally {
        w.close();
    }
    res.text(dec(out));
});
