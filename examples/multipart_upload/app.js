// Streaming multipart uploads — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Visit: http://localhost:3000/   (HTML form)
// Or:    curl -F "user=alice" -F "f=@README.md" http://localhost:3000/upload
//
// The point of this example is the streaming iterator — bytes pulled
// out of the socket on demand, never the whole body in memory at once.
// We hash + size each file part as it streams and return a JSON
// inventory. Persistent storage is intentionally out of scope: see
// `docs/roadmap_next.md §1.5.b` for the `hull/attachment@1` module
// that adds content-addressed disk storage on top of this primitive.

import { app } from "hull:app";
import { crypto } from "hull:crypto";
import { log } from "hull:log";

app.manifest({
    name    : "multipart-upload",
    version : "0.0.1",
    modules : [
        "hull/http-server@1",
        "hull/crypto@1",
        "hull/log@1",
    ],
});

const FORM_HTML = `<!doctype html>
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
`;

// QuickJS doesn't bundle TextDecoder; tiny ASCII helper for text-field
// bodies (form-encoded fields are 7-bit ASCII in this example).
function asciiOf(buf) {
    const u8 = new Uint8Array(buf);
    let s = "";
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    return s;
}

app.get("/", async (_req, res) => {
    res.header("Content-Type", "text/html; charset=utf-8");
    res.text(FORM_HTML);
});

app.get("/health", async (_req, res) => {
    res.json({ status: "ok", runtime: "quickjs" });
});

app.post("/upload", async (req, res) => {
    const parts = [];

    for await (const part of req.multipart()) {
        if (part.filename) {
            // File field: stream chunks AND hash incrementally. Bytes
            // never accumulate — each chunk is fed straight into the
            // hasher and dropped. Memory use stays O(chunk_size) no
            // matter how big the upload.
            const hasher = crypto.createSha256();
            let total = 0;
            for await (const chunk of part.chunks()) {
                total += chunk.byteLength;
                hasher.update(chunk);
                if (total > 5 * 1024 * 1024) {
                    res.status(413);
                    res.json({
                        error      : "file too large",
                        limit_bytes: 5 * 1024 * 1024,
                        name       : part.name,
                        filename   : part.filename,
                    });
                    return;
                }
            }
            parts.push({
                kind        : "file",
                name        : part.name,
                filename    : part.filename,
                contentType : part.contentType,
                size        : total,
                sha256      : hasher.digest(),
            });
        } else {
            const buf = await part.read();
            parts.push({
                kind  : "text",
                name  : part.name,
                value : asciiOf(buf),
            });
        }
    }

    log.info(`[upload] ${parts.length} parts`);
    res.json({ ok: true, parts });
}, {
    multipart : {
        maxPartSize  : 5  * 1024 * 1024,
        maxTotalSize : 16 * 1024 * 1024,
        maxParts     : 32,
    },
});
