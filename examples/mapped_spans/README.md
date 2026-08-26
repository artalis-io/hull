# mapped_spans - reference app for Hull mapped spans

Attaches a host-mapped, read-only file window to a WASM compute plugin via the
public `spans={}` API and reads it at native speed using the `hull_span.h` SDK.

- `compute/spanreader/spanreader.c` - the plugin. Uses **only** the public
  `hull_span.h` API (`hull_span_setup`, `hull_span_find`, `HullSpan`) - no
  hand-written `host_call`, no wire-offset decoding. It resolves a caller-supplied
  span name, does a **bounded** read of the window, and returns a deterministic
  text line. Unknown names and out-of-range offsets are handled explicitly.
- `app.lua` / `app.js` - map a **non-page-aligned** window of `data.bin`
  (file offset 8195, length 4096), attach it as span `source`, and call the
  plugin: `GET /read?name=<span>[&off=<n>]`.
- `data.bin` - deterministic fixture (`byte[i] = i & 0xff`).

## Run

```sh
hull compute build spanreader      # compile the plugin to compute/spanreader.wasm
hull app.lua -p 3000               # or: hull app.js -p 3000
curl 'localhost:3000/read?name=source'          # ok name=source len=4096 foff=8195 off=0 val=3
curl 'localhost:3000/read?name=source&off=100'  # ok ... off=100 val=103
curl 'localhost:3000/read?name=nope'            # unknown name=nope count=1
curl 'localhost:3000/read?name=source&off=5000' # range name=source len=4096 off=5000
```

Refresh the SDK headers after a Hull upgrade with `hull compute refresh-header`.
