# Streaming multipart uploads

`req:multipart()` (Lua) and `req.multipart()` (JS) expose
`multipart/form-data` request bodies as an iterator over parts. Routes
opt in at registration; the handler runs **before** the body is fully
buffered and drives parsing on demand. There is no `req.body` to read
— bytes are pulled out of the socket as the handler asks for them.

> **Scope.** This document covers the iterator API only. The
> attachment-storage layer (`hull/attachment@1`: content-addressed
> dedup, refcount GC, MIME sniffing, manifest-declared write dirs) is
> a separate stdlib module landing in a later release on top of this
> primitive — see `docs/roadmap_next.md §1.5.b`. Until then, app code
> that wants to persist uploads writes them with the iterator directly,
> using `fs.write` and a manifest-declared write path.

## When to use it

Use streaming-multipart routes when:

- the body is `multipart/form-data` (HTML form uploads, `curl -F`, the
  `FormData` API, HTMX `hx-encoding="multipart/form-data"`); and
- one or more parts is a file, OR the total body could be too large to
  hold in memory.

For everything else — JSON, plain forms, small bodies — the regular
buffered-body path is simpler: leave `opts.multipart` off and read
`req.body` as a string.

## Route registration

Declare `opts.multipart` on the route to opt in:

```lua
app.post("/upload", function(req, res)
    for part in req:multipart() do
        -- ...
    end
    res:json({ ok = true })
end, { multipart = {
    max_part_size    = 64 * 1024 * 1024,  -- per-part cap (bytes)
    max_total_size   = 256 * 1024 * 1024, -- whole body cap
    max_parts        = 32,                -- number of parts
    max_headers_size = 8 * 1024,          -- per-part headers
    max_input_buffer = 1 * 1024 * 1024,   -- parser-internal buffer
} })
```

```javascript
app.post("/upload", async (req, res) => {
    for await (const part of req.multipart()) {
        // ...
    }
    res.json({ ok: true });
}, { multipart: {
    maxPartSize    : 64 * 1024 * 1024,
    maxTotalSize   : 256 * 1024 * 1024,
    maxParts       : 32,
    maxHeadersSize : 8 * 1024,
    maxInputBuffer : 1 * 1024 * 1024,
} });
```

JS accepts both snake_case and camelCase cap names; snake_case wins if
both are present. Every cap defaults to `0` (unlimited) when omitted —
**always set sensible caps for adversarial input.** Exceeding any cap
mid-stream raises a parser error which the iterator surfaces to the
handler (see "Errors" below).

A non-streaming POST/PUT route with no `opts.multipart` declared
behaves exactly as before: `req.body` is the buffered body string,
even for multipart content types — the parser is not invoked.

## Reading parts

### Lua

```lua
for part in req:multipart() do
    print(part.name, part.filename, part.content_type)

    if part.filename then
        -- File field — stream chunks and hash incrementally
        local hasher = crypto.create_sha256()
        local total = 0
        for chunk in part:chunks() do
            total = total + #chunk
            hasher:update(chunk)
        end
        log.info(string.format("%s: %d bytes, sha256=%s",
            part.filename, total, hasher:digest()))
    else
        -- Text field — read the whole body
        local value = part:read()
        log.info(part.name .. " = " .. value)
    end
end
```

### JavaScript

```javascript
for await (const part of req.multipart()) {
    console.log(part.name, part.filename, part.contentType);

    if (part.filename) {
        const hasher = crypto.createSha256();
        let total = 0;
        for await (const chunk of part.chunks()) {
            total += chunk.byteLength;
            hasher.update(chunk);
        }
        log.info(`${part.filename}: ${total} bytes, sha256=${hasher.digest()}`);
    } else {
        const buf = await part.read();
        // buf is an ArrayBuffer; decode via TextDecoder for text fields,
        // or use the bytes directly for binary fields.
        log.info(`${part.name} = ${new TextDecoder().decode(buf)}`);
    }
}
```

> The `crypto.create_sha256()` / `crypto.createSha256()` hasher used above is
> the incremental SHA-256 API — `update(chunk)` repeatedly, then
> `digest()` once for the 64-char hex digest. Memory use stays
> O(chunk_size) regardless of how large the upload is. The one-shot
> `crypto.sha256(buf)` still exists for when you already have the
> whole input in hand.

> **Note:** QuickJS does NOT bundle `TextDecoder` by default. For ASCII
> text fields the simplest decode is a manual loop:
>
> ```javascript
> function ascii(buf) {
>     const u8 = new Uint8Array(buf);
>     let s = "";
>     for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
>     return s;
> }
> ```

## Part fields

Every part exposes:

| Field           | Type                | Notes                                                |
|-----------------|---------------------|------------------------------------------------------|
| `name`          | string              | The form field name (Content-Disposition `name=`).   |
| `filename`      | string \| nil/null  | `filename=` if present, `nil`/`null` for text fields.|
| `content_type`  | string \| nil/null  | The part's `Content-Type:` header.                   |
| (JS)            | `contentType`       | camelCase in JS; snake_case in Lua.                  |

## Reading bytes

Two ways to consume a part's body:

| Method                    | Returns                       | When to use                              |
|---------------------------|-------------------------------|------------------------------------------|
| Lua: `part:read()`        | Lua string (byte-clean)       | Small text/binary fields, ≤ a few MB.    |
| Lua: `part:chunks()`      | iterator over byte strings    | Large files; stream to disk / hash.      |
| JS: `await part.read()`   | `ArrayBuffer` (binary-safe)   | Same as Lua `read()`.                    |
| JS: `part.chunks()`       | async iterator over `ArrayBuffer` | Same as Lua `chunks()`.              |

`chunks([min_bytes])` accepts an optional advisory minimum-chunk-size
hint; in this release the hint is accepted-and-ignored and each parser
event surfaces as one chunk (coalescing arrives in a follow-up).

### Binary safety

Both runtimes return raw bytes — no UTF-8 encoding, no truncation,
no normalization:

- **Lua** strings are byte arrays. `#chunk` is the byte count.
  `crypto.sha256(chunk)` over arbitrary binary input returns the
  correct hash.
- **JS** chunks and `read()` results are `ArrayBuffer`, not JS
  strings. Use `.byteLength` for the size; use `new Uint8Array(buf)`
  to access bytes; pass directly to `crypto.sha256(buf)` for hashing.

Decoding text fields:

| Runtime | API                                                                |
|---------|--------------------------------------------------------------------|
| Lua     | `part:read()` returns a string ready to use.                       |
| JS      | `new TextDecoder().decode(buf)` (BYOP — QuickJS doesn't bundle it; the manual ASCII loop above is fine for form-encoded text). |

## Auto-drain

If the handler doesn't read a part's body (skips `read()` and never
iterates `chunks()`), the next `iter.next()` call auto-drains the
leftover `PART_DATA` events before advancing to the next
`PART_BEGIN`. This means you can iterate part metadata without
touching bodies:

```lua
for part in req:multipart() do
    log.info("got part: " .. part.name)
    -- skip body; iterator drains it on next iteration
end
```

## Part lifetime

`Part` (and the JS object returned from `req.multipart()`) is valid
**only until the next `iter.next()` call.** The parser is forward-only
and there is no way to rewind; holding a part across the next iteration
and then calling `part.read()` or `part.chunks()` on it will read from
whatever part is current — almost certainly the wrong thing.

In practice this is the natural shape of a `for ... of` loop, so it
takes effort to misuse. Don't stash the part in an outer-scope
collection and read it later.

## Errors

The parser can emit `ERROR` at any time — typically when a per-part or
total-body cap is exceeded, or when the body itself is malformed
(missing boundary, truncated mid-part).

- **Lua** — `iter.next()` / `part.read()` / `chunks.next()` raises a
  Lua error with `req:multipart(): parser error (code N)` where `N` is
  the Keel `KL_MP_ERR_*` code. Bubbles up through your handler unless
  you `pcall` it; dispatch writes a 500 with `Internal Server Error`.
- **JS** — the awaited Promise is rejected with an `Error` whose
  message is `multipart: parser error`. Catch it with try/catch around
  the `for await` body to send a user-facing 4xx, or let it bubble for
  the default 500.

Common parser error codes (see `vendor/keel/include/keel/body_reader_multipart.h`):

| Code | Meaning |
|---|---|
| `KL_MP_ERR_MALFORMED`        | Bad preamble / boundary / header line. |
| `KL_MP_ERR_PART_TOO_LARGE`   | Per-part body cap exceeded. |
| `KL_MP_ERR_TOTAL_TOO_LARGE`  | Whole-body cap exceeded. |
| `KL_MP_ERR_TOO_MANY_PARTS`   | Part-count cap exceeded. |
| `KL_MP_ERR_HEADERS_TOO_LARGE`| Per-part header bytes exceeded. |
| `KL_MP_ERR_BUFFER_OVERFLOW`  | Parser-internal buffer cap exceeded. |

## Manifest interaction

`opts.multipart` is the entire opt-in — no special manifest flag is
needed. The iterator pulls bytes off the socket; how (or whether) the
handler persists those bytes is a separate question.

`hull/attachment@1` (a later stdlib module — see
[`docs/roadmap_next.md` §1.5.b](roadmap_next.md)) will wrap this
iterator with content-addressed disk storage, dedup, MIME sniffing,
and refcount GC, and that module will be the one declaring
`fs.write` against a manifest-allowlisted directory. Until it lands,
applications that need to persist uploads have to bring their own
storage layer — Redis, S3, or whatever — and use the iterator as the
ingest primitive.

The host allowlist (`manifest.hosts`) is unrelated — inbound HTTP
isn't constrained by host allowlists.

## Constraints + known limitations

- **Live connection required.** The iterator parks the handler on
  `NEED_DATA` and resumes from the socket-read callback. The in-process
  test harness (`hull test` / `test.post(...)`) has no socket, so the
  first `NEED_DATA` raises a clear error. End-to-end coverage for these
  routes lives in `tests/e2e_multipart.sh` — they need to run against a
  real `hull dev` or built binary.
- **One iterator per request.** Calling `req.multipart()` more than
  once per request returns iterators that share parser state — the
  first iterator consumes; subsequent iterators see `DONE`.
- **Mid-stream connection close.** If the client disconnects while
  the handler is awaiting bytes, the parked continuation currently
  leaks. The handler's coroutine/Promise stays alive in the runtime
  until process shutdown. Production deployments should run behind a
  reverse proxy that enforces request timeouts as a defensive layer.
  A follow-up will wire `on_destroy` of the wrapper to cancel the
  continuation.
- **`chunks(n)` hint is currently advisory.** Each parser event yields
  one chunk; small chunks aren't coalesced. A future revision can
  buffer to a minimum size.
- **No transparent decompression.** A part sent with
  `Content-Encoding: gzip` is delivered as compressed bytes; the
  application is responsible for decoding.

## Worked example

A complete runnable demo lives in `examples/multipart_upload/` —
identical Lua and JS routes that accept text fields and file uploads,
write files to `data/uploads/`, and respond with a JSON inventory.

To run:

```sh
cd examples/multipart_upload
hull app.lua -p 3000   # or: hull app.js -p 3000
curl -F "user=alice" -F "f=@README.md" http://localhost:3000/upload
```

## Internals

- **Phase 1** (cap-layer): a Hull wrapper around Keel's
  `kl_body_reader_multipart` adds a single-shot "parked handler" slot
  that fires when more body bytes arrive
  (`src/hull/cap/body.c::hl_cap_multipart_factory`).
- **Phase 2 Slice 1** (route plumbing): `app.<verb>(..., { multipart })`
  registers the route via `kl_server_route_streaming` and a per-runtime
  factory shim that allocates the `KlMultipartConfig` from
  `opts.multipart`.
- **Phase 2 Slice 2/3** (iterator): the Lua + JS iterators each call
  `kl_multipart_next()`, set `c->state = KL_CONN_READING_BODY` before
  yielding on `NEED_DATA`, and resume from
  `hl_cap_multipart_park`'s callback. The yield/resume helper bypasses
  the standard `kl_async_suspend` machinery because the resume trigger
  is the body-reader callback, not `kl_async_complete`.

The end-to-end test suite (`tests/e2e_multipart.sh`) covers the
synchronous fast path, NEED_DATA cycles up to 5 MB, mixed bodies,
keep-alive bursts, max-part-size enforcement, and the auto-drain path.
