# multipart_upload - streaming file uploads

A minimal demonstration of `req:multipart()` (Lua) / `req.multipart()`
(JS). Accepts `multipart/form-data` bodies, streams parts on demand,
hashes each file part as bytes arrive, and returns a JSON inventory.

See [`docs/multipart.md`](../../docs/multipart.md) for the full API
documentation.

## Run

```sh
# Lua
hull app.lua -p 3000

# JS
hull app.js -p 3000
```

Then open `http://localhost:3000/` for the HTML form, or use curl:

```sh
curl -F "user=alice" -F "tag=demo" -F "f=@README.md" \
    http://localhost:3000/upload
```

Sample response:

```json
{
    "ok": true,
    "parts": [
        { "kind": "text", "name": "user", "value": "alice" },
        { "kind": "text", "name": "tag",  "value": "demo" },
        { "kind": "file", "name": "f",
          "filename": "README.md",
          "content_type": "application/octet-stream",
          "size": 1234,
          "sha256": "ab12…" }
    ]
}
```

## What's demonstrated

- Route opt-in via `opts.multipart` with per-part / total / parts caps.
- Iterating parts (text + file fields, in body order).
- Streaming file bodies via `part:chunks()` / `part.chunks()`, hashing
  on the fly with `crypto.sha256`.
- Reading text fields via `part:read()` / `await part.read()`.
- Per-file size cap enforced inside the handler - the parser's
  `max_part_size` is the outer wall; the handler's running total is
  the inner one (so the response can be a structured 413 instead of a
  500).

## What's NOT demonstrated

- **On-disk persistence.** This example holds bytes in memory long
  enough to compute a hash and then drops them - there's intentionally
  no `fs.write` call. The `hull/attachment@1` stdlib module (later
  release) wires content-addressed storage, dedup, a metadata table,
  and refcount GC on top of this iterator. See
  [`docs/roadmap_next.md` §1.5.b](../../docs/roadmap_next.md).
- **MIME validation / content sniffing.** This example trusts the
  client's per-part `Content-Type` header. Production code should
  either restrict the accepted MIMEs or sniff the body's magic bytes
  before acting on them.
- **Auth.** Anyone who can reach the port can upload.

## Testing

The streaming-iterator path needs a live socket (the in-process test
harness has no connection to park against), so the unit-test style
`hull test` flow doesn't cover the upload route. Real coverage lives
in `tests/e2e_multipart.sh`, which spins up real `hull` processes for
both runtimes and drives them with `curl -F`. Run from the repo root:

```sh
make e2e-multipart
```
