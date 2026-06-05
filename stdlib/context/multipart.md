<!-- minimal -->
## Multipart Uploads

Streaming `multipart/form-data` parser. Routes opt in via
`{ multipart = {...} }`; handlers pull parts from an iterator. No
`req.body` for these routes — the iterator IS the body.

```lua
-- Lua
app.post("/upload", function(req, res)
    for part in req:multipart() do
        if part.filename then
            local bytes = part:read()           -- whole-part buffer
            -- or: for chunk in part:chunks() do ... end  -- stream
        else
            local value = part:read()           -- text field
        end
    end
    res:json({ ok = true })
end, { multipart = { max_part_size = 64 * 1024 * 1024 } })
```

```javascript
// JS
app.post("/upload", async (req, res) => {
    for await (const part of req.multipart()) {
        if (part.filename) {
            const buf = await part.read();      // ArrayBuffer
            // or: for await (const chunk of part.chunks()) { ... }
        } else {
            const buf = await part.read();
        }
    }
    res.json({ ok: true });
}, { multipart: { maxPartSize: 64 * 1024 * 1024 } });
```

No `manifest.modules` entry needed — multipart is a routing option, not
a module. `hull/http-server@1` covers it.

<!-- compact -->
## Caps + error handling

All five caps default to `0` (unlimited). Exceeding any of them mid-
stream raises a parser error inside the iterator; wrap with `pcall` /
`try-catch` to write a structured 4xx response. Uncaught errors → 500.

| Cap | Lua | JS | Effect |
|---|---|---|---|
| Per-part body | `max_part_size` | `maxPartSize` | `PART_TOO_LARGE` when a single part exceeds |
| Whole body | `max_total_size` | `maxTotalSize` | `BODY_TOO_LARGE` over all parts |
| Part count | `max_parts` | `maxParts` | `TOO_MANY_PARTS` on the (N+1)th `PART_BEGIN` |
| Per-part headers | `max_headers_size` | `maxHeadersSize` | `HEADERS_TOO_LARGE` while parsing a part header |
| Input buffer | `max_input_buffer` | `maxInputBuffer` | Hard cap on parser scratch buffer |

```lua
app.post("/upload", function(req, res)
    local files = {}
    local ok, err = pcall(function()
        for part in req:multipart() do
            files[#files + 1] = { name = part.name, filename = part.filename }
        end
    end)
    if not ok then
        res:status(413):json({ ok = false, error = tostring(err) })
        return
    end
    res:json({ ok = true, files = files })
end, { multipart = { max_parts = 16, max_total_size = 32 * 1024 * 1024 } })
```

```javascript
app.post("/upload", async (req, res) => {
    const files = [];
    try {
        for await (const part of req.multipart()) {
            files.push({ name: part.name, filename: part.filename });
        }
    } catch (e) {
        res.status(413).json({ ok: false, error: String(e.message || e) });
        return;
    }
    res.json({ ok: true, files });
}, { multipart: { maxParts: 16, maxTotalSize: 32 * 1024 * 1024 } });
```

Works for both single-read and multi-read bodies — Keel v2.2.0
dispatches the handler BEFORE feeding leftover body bytes, so the
handler is alive when a cap trips inside `on_data`.

## Part fields + binary safety

- `part.name` — form field name (always set)
- `part.filename` — `nil`/`null` for text fields, string for file uploads
- `part.content_type` (Lua) / `part.contentType` (JS) — from
  `Content-Type` header

Reads are binary-safe: Lua returns byte-clean strings (`#chunk = bytes`),
JS returns `ArrayBuffer` (never JS strings — would UTF-8-mangle binary
input). To decode text fields in JS use `new TextDecoder().decode(buf)`
(QuickJS doesn't bundle it — supply your own polyfill or use a manual
ASCII loop for known-ASCII fields).

## Incremental SHA-256 (`crypto.create_sha256` / `crypto.createSha256`)

Stream-hash parts without buffering. Chainable `update`, one-shot
`digest` (lowercase hex). Update-after-digest and double-digest both
raise.

```lua
local h = crypto.create_sha256()
for chunk in part:chunks() do h:update(chunk) end
local sha = h:digest()
```

```javascript
const h = crypto.createSha256();
for await (const chunk of part.chunks()) h.update(chunk);
const sha = h.digest();
```

<!-- full -->
## Hash + size inventory (no disk persistence)

The iterator returns bytes; persistent storage isn't in the stdlib
yet (roadmap §1.5.b-4 — `hull/attachment@1`, content-addressed disk
storage). Until that ships, the realistic pattern is to hash + size
each part as it streams and emit a JSON inventory. Memory stays
O(chunk_size) regardless of upload size — `crypto.create_sha256` is
the streaming digest; the bytes themselves are dropped after each
update.

```lua
app.post("/upload", function(req, res)
    local files, fields = {}, {}
    local ok, err = pcall(function()
        for part in req:multipart() do
            if part.filename then
                local h, size = crypto.create_sha256(), 0
                for chunk in part:chunks() do
                    h:update(chunk)
                    size = size + #chunk
                end
                files[#files + 1] = {
                    name = part.name, filename = part.filename,
                    content_type = part.content_type,
                    size = size, sha256 = h:digest(),
                }
            else
                fields[part.name] = part:read()
            end
        end
    end)
    if not ok then
        res:status(413):json({ ok = false, error = tostring(err) })
        return
    end
    res:json({ ok = true, fields = fields, files = files })
end, { multipart = {
    max_parts      = 32,
    max_part_size  = 16 * 1024 * 1024,
    max_total_size = 64 * 1024 * 1024,
} })
```

```javascript
app.post("/upload", async (req, res) => {
    const files = [], fields = {};
    try {
        for await (const part of req.multipart()) {
            if (part.filename) {
                const h = crypto.createSha256();
                let size = 0;
                for await (const chunk of part.chunks()) {
                    h.update(chunk);
                    size += chunk.byteLength;
                }
                files.push({
                    name: part.name, filename: part.filename,
                    contentType: part.contentType,
                    size, sha256: h.digest(),
                });
            } else {
                const buf = await part.read();
                // Text fields are still ArrayBuffers — decode as needed.
                let s = "";
                const u8 = new Uint8Array(buf);
                for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
                fields[part.name] = s;
            }
        }
    } catch (e) {
        res.status(413).json({ ok: false, error: String(e.message || e) });
        return;
    }
    res.json({ ok: true, fields, files });
}, { multipart: { maxParts: 32, maxPartSize: 16*1024*1024, maxTotalSize: 64*1024*1024 } });
```

Manifest:

```lua
app.manifest({
    modules = { "hull/http-server@1", "hull/crypto@1" },
})
```

## Auth rejection BEFORE iterating

If the handler responds without ever calling `req:multipart()` /
`req.multipart()` (e.g. auth fails), Keel forces `keep_alive=0` so
stranded body bytes don't bleed into the next request on a re-used
connection. No special handling needed in user code:

```lua
app.post("/upload", function(req, res)
    if not req.ctx.user then
        res:status(401):json({ error = "unauthorized" })
        return                                 -- never touches multipart
    end
    for part in req:multipart() do ... end
end, { multipart = { ... } })
```

Verified end-to-end by `tests/e2e_multipart.sh` scenario 13b
(`/upload-sync-reject`): the response is sent with `Connection: close`
implicit (Keel only emits `Connection: keep-alive` when keep-alive is
on), proving the dispatch layer correctly stranded the body.

## Forwarding to WASM compute

Stream-feed each chunk into a compute module rather than buffering:

```lua
local m = compute.instance("upload_processor", { heap = 4 * 1024 * 1024 })

app.post("/process", function(req, res)
    local results = {}
    for part in req:multipart() do
        if not part.filename then goto continue end
        for chunk in part:chunks() do
            m:call(chunk)                   -- streams into linear memory
        end
        results[#results + 1] = m:call("")  -- empty chunk = finalize
        ::continue::
    end
    res:json({ ok = true, results = results })
end, { multipart = { max_part_size = 32 * 1024 * 1024 } })
```

The persistent compute instance retains state across calls (its linear
memory survives between invocations), so it can accumulate work
across many small chunks without per-call instantiation cost. See
`hull agent context --task=compute --level=compact` for the compute-
side contract.

## Part lifecycle gotchas

- **Forward-only.** A `part` is invalidated as soon as the iterator
  advances. Stash `part.name` / `part.filename` early; don't keep
  references to `part` across iter steps.
- **One iterator per request.** Calling `req:multipart()` /
  `req.multipart()` more than once returns iterators that share parser
  state — the first one consumes; the rest see `DONE`.
- **Auto-drain.** Not reading a part's body (no `:read()` / `.read()`,
  no `chunks` loop) is fine — the iterator drains pending `PART_DATA`
  events before advancing to the next part.
- **`chunks(n)` hint is advisory.** Each parser event yields one
  chunk; the `n` minimum-bytes hint isn't enforced yet (coalescing is
  a follow-up).

## Testing

Multipart routes need a live connection — in-process `hull test`
dispatch raises on the first `NEED_DATA`. End-to-end coverage lives in
`tests/e2e_multipart.sh` (run `make e2e-multipart`); patterns to copy:
the cap-rejection scenarios use `pcall` / try-catch + assert on the
status code and the JSON error body.

## See also

- `docs/multipart.md` — full architecture: parser, parking, dispatch
  contract, Keel version trail
- `examples/multipart_upload/` — runnable Lua + JS demo
- `crypto.create_sha256` / `crypto.createSha256` — also useful outside
  multipart for any streaming digest
