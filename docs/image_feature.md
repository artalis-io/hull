# Image codecs as a composable feature

The image codec subsystem (`hull/image`: decode/encode PNG/JPEG/BMP, raw pixel
buffers, the `HlImage` userdata) is an **embedded, auto-composed feature** on the
native base — the same mandatory-composition axis as the runtimes, HTTP, WASM, and
the SQLite udf bridge (see CLAUDE.md "Composable runtime + HTTP base"). The native
base platform lib is **image-less**; `hull build` composes the codec back only for
apps that declare `hull/image`. You never `hull feature install` it — the archives
ship inside the distributed `hull` and compose automatically.

This is the [Step 1 slice](roadmap.md) of the broader "flavors become feature
presets" direction: image was the cleanest remaining base-resident subsystem to
extract, and it validates the auto-compose mechanism for a small, self-contained
codec.

## What moves where

| Piece | From | To (composable archive) |
|-------|------|-------------------------|
| codec vtable + stb backend + vendored stb | base `libhull_platform.a` | `libhull_feature-image.a` (runtime-agnostic core) |
| the `image.*` binding + `HlImage` userdata (`mod_image`) | the per-runtime base archive | `libhull_feature-image-<rt>.a` |

Both are embedded in hull (`embedded_image.h`, via the Makefile's
`EMBEDDED_IMAGE_H`) and extracted + whole-archived at `hull build`, exactly like
the WASM core + compute bridge.

## The seam

`HL_ENABLE_IMAGE` stays **defined** in the base (like `HL_ENABLE_WASM`), so:

- the resolver keeps reporting `HL_MOD_CAP_IMAGE` (an image app passes module
  resolution on the base), and
- `modules.c`'s registration + `mod_gpu`'s `#ifdef HL_ENABLE_IMAGE` texture paths
  compile unchanged.

The engine objects are filtered out of the base and replaced by weak stubs:

- **cap-level** (`src/hull/image_weakstub.c`, runtime-agnostic, in the base
  platform lib): weak `hl_image_new` / `hl_image_free`, referenced by
  `mod_gpu`'s `gpu.texture_read` (which builds an `HlImage` to return). When the
  image feature is composed the strong `cap/image.c` defs win; when not, the weak
  no-ops satisfy the link (`gpu.texture_read` fails closed — returns nothing).
- **per-runtime** (`src/hull/runtime/{lua,js}/image_stub.c`, in the runtime
  feature archive): weak `luaopen_hull_image` (Lua) / `hl_js_init_image_module`
  (JS), referenced by `modules.c`'s unconditional registration. The strong
  `mod_image.o` (in the composed bridge) overrides.

Mirrors `wasm_weakstub.c` + `runtime/{lua,js}/wasm_stub.c` exactly.

## The gate

`hull build` composes the image core + the per-runtime bridge iff the resolved
manifest declares `hull/image` (`req_caps & HL_MOD_CAP_IMAGE`, exposed as
`needs_image` from `tool.modules_resolve`, alongside `needs_http`/`needs_wasm`/
`needs_sqlite`). Unlike WASM, no second signal is needed: the only reachable
`HlImage` producer is the `image` module itself, so a declared `hull/image` is the
whole gate.

- A genuinely image-free app links **zero** stb (~146 KB smaller). Verify:
  `nm app | grep stbi_load_from_memory` → empty.
- The base defines zero image engine symbols. Verify:
  `nm libhull_platform.a | grep stbi_load_from_memory` → empty (only the
  `image_weakstub` weak `hl_image_new` remains).

## `HL_ENABLE_IMAGE=0` (the subtractive knob) still works

The composable path is the default `HL_ENABLE_IMAGE=1` base. `make
HL_ENABLE_IMAGE=0` is the older **subtractive** flavor: it drops `cap/image` +
stb + `mod_image` at compile time (there is no feature to compose), for a
compute/CLI/signing binary that never touches images. `hull/image` then needs an
optional `"hull/image@1?"` declaration to resolve (a non-optional decl is a hard
load error). The image feature archives are gated out of the build under
`HL_ENABLE_IMAGE=0` (`IMG_FEATURE_*` resolve empty), so `make HL_ENABLE_IMAGE=0`
never tries to build them from the (filtered-out) `cap_image.o`.

## Cosmo

Cosmo can't force-load native feature archives, so its fat-APE base keeps the
image codec compiled in (like the runtimes, HTTP, and WASM). The image-less base
+ compose model is native-only.

## Composed-feature signing

The three image archives ship inside the distributed `hull`, so they land in the
`platform_domain` of `package.sig.gethull.composed` (platform-key attested,
runtime §5c FATAL on tamper) — the same trust chain as the runtime/HTTP/WASM
archives. `release.yml` builds + signs the `image` / `image-lua` / `image-js`
stems into the platform manifest and embeds those exact bytes
(`TRUST_FEATURE_LIBS=1`). See [docs/composed_feature_signing.md](composed_feature_signing.md).

## Tests

`tests/e2e_feature_image.sh` (`make e2e-feature-image`): asserts the base is
image-less (0 stb symbols), an image app (Lua + JS) composes the feature and a
create → encode-PNG → decode round-trip runs in the produced binary, and an
image-free app links zero stb and still runs. The subtractive `HL_ENABLE_IMAGE=0`
link is covered by the `flavors` CI matrix.
