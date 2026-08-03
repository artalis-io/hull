# Building Hull on musl (Alpine)

Hull builds and runs on musl libc (Alpine Linux), producing binaries for
minimal `scratch`/Alpine containers. Hull's own CI and dev happen on glibc +
macOS, so musl's stricter runtime is a useful second correctness check - it has
already caught a latent bug the glibc loader silently tolerated.

## What it takes: essentially nothing

The vendored stack (mbedTLS, SQLite, WAMR, QuickJS, Lua, Keel, TweetNaCl,
stb, sh_arena, sh_json) compiles clean under musl with **zero** source changes.
Hull needs exactly **one** build shim plus **one** correctness fix that already
landed:

1. **`__O_TMPFILE` (build shim).** `vendor/pledge/libc/calls/pledge-linux.c`
   references the glibc-only macro `__O_TMPFILE` - purely as the numeric
   seccomp-mask constant `020000000` (the Linux ABI flag value, identical on
   x86_64 and aarch64; the file's own comments spell it out). glibc defines it
   via `<bits/fcntl-linux.h>`; musl does not. `mk/platform/linux.mk` detects a
   musl target (`cc -dumpmachine` contains `musl`) and adds
   `-D__O_TMPFILE=020000000` to `PLEDGE_CFLAGS` only - scoped to the one file
   that uses it, and only on musl (a glibc `-D` would just warn on redefine).

2. **`.data.rel.ro` emitter placement (landed in the ELF object emitter).**
   The compiler-free emitter (`src/hull/obj_emit.c`) put the relocated
   `hl_app_entries[]` array in `.rodata` (read-only). musl's loader SIGSEGVs
   applying the RELATIVE relocations into a read-only page; glibc tolerates the
   resulting text relocations. The fix emits it into `.data.rel.ro`
   (RELRO-eligible), which is what `cc` does for a `const T[]` of pointers - and
   the correct hardening posture. See the commit for
   `fix(obj_emit): emit the app-registry into .data.rel.ro, not .rodata`.

That is the whole port. No musl-specific `#ifdef` in Hull source, no forked
platform code.

## Building

On an Alpine host (or `docker run alpine`):

```sh
apk add --no-cache build-base clang lld make xxd bash git perl linux-headers
make -j"$(nproc)"          # the hull binary
make platform -j"$(nproc)" # libhull_platform.a for `hull build`
```

`cc` on Alpine is a musl-targeting gcc, so `-dumpmachine` reports
`<arch>-alpine-linux-musl` and the `__O_TMPFILE` shim engages automatically. No
`CC=` override or manual `-D` is needed.

Apps then build through the normal (compiler-free emit) path and produce
musl-linked binaries:

```sh
hull build ./myapp -o ./myapp/bin
```

## What is verified

`tests/e2e_musl.sh` (`make e2e-musl`, and the `musl (Alpine)` CI job) builds hull
under musl and then, through the default emit path, builds and runs:

- a **compute `app.main`** app - the regression lock for the `.data.rel.ro` fix
  (this binary used to SIGSEGV in `ld-musl`'s `do_relocs` before `main`);
- an **HTTP server** app - proves the full server runtime serves a request;
- a **fully static** app via `--linker=lld-static` (Tier B) - see below.

The script is dual-mode: on a musl host it builds directly; on a non-musl host
(dev macOS/glibc, CI ubuntu) it re-execs itself inside an `alpine:3.20` Docker
container. That is why the CI job runs on a plain ubuntu runner rather than
`container: alpine` - it sidesteps `actions/checkout`'s glibc-node breakage on an
Alpine container while still exercising a real musl build.

## Fully static binaries (Tier B, `--linker=lld-static`)

On musl, `hull build --linker=lld-static ./app` produces a **fully static**
executable (no interpreter, runs on bare `scratch`) by invoking `ld.lld`
directly - no cc driving the link. Beyond the emitted object it needs `ld.lld`
(from `lld`), the musl floor (`crt1.o`/`crti.o`/`crtn.o`/`libc.a`, from
`musl-dev`), and the **compiler runtime** `libgcc.a` (soft-float builtins the app
archives reference, e.g. `__multf3`). Both the floor and `libgcc.a` are resolved
**without a compiler**, in this order:

- **Floor** (`crt*.o` + `libc.a`): `HULL_LIBC_DIR` → an installed bundle
  `~/.hull/tools/libc-musl-<arch>/` → `/usr/lib` (the musl-dev default).
- **libgcc.a**: `HULL_LIBGCC` → the installed bundle → a glob of the system gcc
  runtime (`/usr/lib/gcc/*/*/libgcc.a`, no cc spawn) → `cc
  -print-libgcc-file-name` (last resort, the only cc-needing path).

So Tier B is **fully self-contained** on any Alpine with `build-base` (the glob
finds the system `libgcc`; cc is never spawned), and on a box with **nothing**
but hull + `ld.lld` once the bundle is present. Two ways to get the bundle:

```sh
# 1. Fetch a prebuilt, Ed25519-signed bundle from Hull's release (no musl host):
hull tools install libc-musl-$(uname -m)   # -> ~/.hull/tools/libc-musl-<arch>/
# 2. Or assemble it yourself on a musl host:
make floor-musl                            # (wrapper over scripts/build_musl_floor.sh)

hull build --linker=lld-static ./app       # no musl-dev, no gcc, no cc needed
```

The bundle holds `crt*.o` + `libc.a` + the stub `libm`/`libpthread` + `libgcc.a`,
packed as a flat ustar. `hull tools install libc-musl-<arch>` fetches
`hull-libc-musl-<arch>.tar` and verifies its SHA-256 against the signed
`hull.sha256` manifest (the same trust chain as `hull tools install wamrc` /
`hull update`) before extracting to `~/.hull/tools/libc-musl-<arch>/`, which the
linker resolves automatically. Published for `linux-x86_64` and `linux-aarch64`
(musl is Linux; macOS/cosmo have no floor). Like `wamrc`, the live install is
exercised post-release via `tests/release_smoke.sh`, not in CI.

`zig cc` (`--linker=zig`) remains the turnkey cross-target alternative (bundles
crt+libc+compiler-rt for ~40 targets); Tier B is the purist `ld.lld` + floor
path. See [docs/toolchain_free_build.md](toolchain_free_build.md). Covered by
`tests/e2e_musl.sh` (asserts static + runs, cc-free, and via the bundle).

## Producing the musl platform archive set

The floor above is only half of a musl static link: `--linker=lld-static` (Tier
B) and `--linker=zig --target=<arch>-linux-musl` (cross) also need Hull's OWN
archives - `libhull_platform.a` + every composed feature archive - built against
musl, because a glibc-built `.a` will not static-link against musl (see the
`linker_lld.c` header). `scripts/build_musl_platform.sh` produces that full set,
the musl counterpart of the floor script:

```sh
# On a musl host (Alpine) with the C toolchain, or via Docker from anywhere:
docker run --rm -v "$PWD":/work:ro -v /tmp/out:/out alpine:3.20 sh -c '
  apk add --no-cache build-base clang lld make xxd bash git perl linux-headers
  sh /work/scripts/build_musl_platform.sh /work /out/hull-libhull-platform-musl-$(uname -m).tar
'
```

It builds `make platform` + `platform-slim` + `feature-embedded` (the exact set
the release's `build-platform-native` job produces, but musl-built) and packs a
flat ustar of `libhull_platform.a`, `libhull_platform-slim.a`, and every
`libhull_feature-*.a`. The build runs in a container-internal copy of the
checkout, so the source may be mounted **read-only** - a host checkout's `build/`
is never clobbered by the in-container musl objects.

## Caveats / not-yet

- **Sandbox in a container.** The e2e runs apps with `--no-sandbox`: Docker's
  default seccomp/landlock restrictions make pledge/unveil enforcement fail
  inside an unprivileged container. That is a container-capability limitation,
  not a musl issue; a musl binary on a real host sandboxes normally.
- **Publishing + consuming the musl archives.** `scripts/build_musl_platform.sh`
  can now PRODUCE the musl archive set (above), but the signed release matrix
  still ships only glibc + cosmo binaries, and `hull build` does not yet select a
  musl platform lib for a `-musl` target (the `#206` guard rejects the cross with
  a clear message). Publishing the set into the signed platform manifest (a new
  `build-platform-musl` release job + a fetch/select path) is the remaining
  half of audit item #4 (docs/build_arc_audit.md).
