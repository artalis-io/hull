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
- an **HTTP server** app - proves the full server runtime serves a request.

The script is dual-mode: on a musl host it builds directly; on a non-musl host
(dev macOS/glibc, CI ubuntu) it re-execs itself inside an `alpine:3.20` Docker
container. That is why the CI job runs on a plain ubuntu runner rather than
`container: alpine` - it sidesteps `actions/checkout`'s glibc-node breakage on an
Alpine container while still exercising a real musl build.

## Caveats / not-yet

- **Static musl (`--linker=lld-static`, Tier B).** A musl platform library now
  exists, which is the precondition Tier B was blocked on. End-to-end validation
  of the direct `ld.lld -static` link on musl is the remaining follow-up before
  Tier B is a supported, tested path. See
  [docs/toolchain_free_build.md](toolchain_free_build.md).
- **Sandbox in a container.** The e2e runs apps with `--no-sandbox`: Docker's
  default seccomp/landlock restrictions make pledge/unveil enforcement fail
  inside an unprivileged container. That is a container-capability limitation,
  not a musl issue; a musl binary on a real host sandboxes normally.
- **Published artifacts.** The signed release matrix ships glibc + cosmo
  binaries. A published, signed musl platform library / binary would be a
  separate release-pipeline decision (new matrix entry + manifest line).
