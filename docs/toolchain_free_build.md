# Toolchain-free `hull build` (embedded linker + cross-compilation)

## Where we are

`hull build` is **compiler-free**: it emits `app_registry.o` directly
(`obj_emit.c`) and links against `libhull_platform.a` + a bundled
`app_main.o`, with no C compiler (docs/compiler_free_build.md). What it is
**not** yet is **linker-free**: `linker_system.c` still drives the system
`cc`/`ld` to perform the link. So the box building a Hull app still needs a
system toolchain for the link step, and it can only build for **its own
platform** (the system `cc` links native).

This epic closes both gaps behind the `HlLinkerVtable` seam
(`include/hull/linker.h`) that Phase 1 of the compiler-free work already put
in place:

1. **Toolchain-free** - side-load `lld` (or `mold`) as a Hull tool so the
   link needs no system toolchain.
2. **Cross-compilation** - emit + link for a **different** (os, arch) than
   the host, so one `hull build` invocation can produce every target.

The emitter half of cross-compilation already works: `obj_emit.c` is
host-independent and emits ELF / Mach-O / COFF for x86-64 or aarch64 from any
host (validated in Phase 1 against `llvm-readelf` / `llvm-readobj`). The link
half is what remains.

## The seam (already built)

`include/hull/linker.h`:

```c
typedef struct { HlObjFormat format; HlObjArch arch; } HlLinkTarget;

typedef struct {
    const char *(*name)(HlLinker*);
    int         (*is_available)(HlLinker*);
    int         (*link)(HlLinker*, const char *out,
                        const char **objs, const char **libs,
                        const HlLinkTarget *tgt);   // <- format/arch already here
    void        (*destroy)(HlLinker*);
} HlLinkerVtable;

HlLinker *hl_linker_select(const char *explicit_linker, const char *hull_exe);
```

`linker_system.c` is today's only backend (invokes `cc`/`ld`; ignores `tgt`
because the native `cc` infers it). `hl_linker_select` already reserves
`hull_exe` for "future embedded lld/mold resolution" and dispatches on an
explicit name. Adding lld/mold is a new backend behind this vtable + a
`--linker=` selector; **no seam change**.

## Axis 1 - the linker as a Hull tool (`hull tools install lld`)

`lld` is LLVM's linker: one binary that is `ld.lld` (ELF), `ld64.lld`
(Mach-O), and `lld-link` (COFF/PE) depending on how it is invoked. `mold` is
a faster ELF/Mach-O linker. Both are far too large to embed in `hull`
(10s of MB), so they follow the **tool** model - exactly like `wamrc`
(docs/tools_install.md): side-loaded, version-coupled to the release,
Ed25519-signed in `hull.sha256`, resolved via `hl_tools_lookup_path`
(`~/.hull/tools` -> `dirname(hull)` -> `$PATH`).

- `hull tools install lld` / `hull tools install mold` - one row each in the
  `TOOLS[]` registry (`src/hull/tools_install.c`), matching `hull-lld-<plat>`
  / `hull-mold-<plat>` assets published by `release.yml`.
- `hull build --linker=lld|mold|system|<path>` - parallels the old
  `--compiler=`. `hl_linker_select` grows an `"lld"`/`"mold"` sentinel that
  resolves the tool and constructs `linker_lld.c` / `linker_mold.c`.
- Default stays `system` for now (lld/mold are opt-in until the crt/libc
  floor below is solved).

## Axis 2 - the per-target floor (the actual work)

Invoking a linker **directly** (not through `cc`) means Hull must supply
everything `cc` silently provides. This is the hard part, and it is what
separates the two tiers:

| Tier | Invocation | Needs a system toolchain? | Floor Hull must supply |
|------|-----------|---------------------------|------------------------|
| **A** | `cc -fuse-ld=lld` (cc drives, lld links) | **Yes** (still needs `cc`) | none - `cc` supplies it |
| **B** | `ld.lld` / `ld64.lld` directly | **No** - toolchain-free | crt + libc + platform stubs |

The Tier B floor, per format:

- **ELF (Linux):** crt startup objects (`Scrt1.o`, `crti.o`, `crtn.o`),
  `libc` + `libm` + `libpthread` (or a static libc), and the dynamic-linker
  path. **Static musl** is the tractable first target: `-static` against a
  bundled `libc.a` + `crt1.o` removes the dynamic-linker + shared-lib
  problem entirely.
- **Mach-O (macOS):** the `libSystem` stub (`.tbd`) + an **ad-hoc code
  signature** (`ld64.lld` emits the signature itself, which is why it is the
  Mach-O linker of choice here).
- **COFF/PE (Windows):** CRT + import libraries for
  `kernel32`/`ws2_32`/`ucrtbase`.
- **Cosmo/APE:** the cosmo crt + `ape.lds` + `apelink` - a separate path,
  out of scope for the first cut.

The floor is **bundle-able** (a small static-musl crt + `libc.a` is a few
hundred KB, and could itself be a `hull tools install libc-<target>` asset),
but it is genuinely per-(os, arch) engineering, not a one-liner. Sizing and
sourcing it is the epic's real cost.

> **`zig cc` (implemented: `--linker=zig`).** Zig bundles a full cross
> toolchain (clang + lld + musl/glibc/mingw crt+headers) for ~40 targets and
> cross-links from any host with `zig cc --target=<triple>`. `linker_zig.c` is
> a `HlLinkerVtable` backend that runs `zig cc [--target=] <objs> <libs>`; the
> compiler-free emitter is untouched (zig only links the emitted
> `app_registry.o`). It delivers Tier B **and** cross-compilation in one tool,
> and - targeting `linux-gnu` - sidesteps the musl port (`__O_TMPFILE` resolves
> under glibc headers). Empirically (macOS/arm64 host, run in Docker): zig
> cross-links Hull's emitted object into **glibc-dynamic** (runs on Ubuntu) and
> **static-musl** (runs on Alpine) Linux binaries, both arches.
>
> **Where zig works vs. not:** great for **Linux native + cross-to-Linux**
> (bundles the complete floor). It does **not** relink a **macOS-native** app:
> Hull's macOS platform lib references the system SDK `.tbd` stubs
> (frameworks/libSystem), which zig's Mach-O linker rejects
> (`failed to parse TBD file: NotLibStub`). So on macOS use `--linker=lld` or
> `--linker=system`; `--linker=zig` targets Linux. It needs the
> target-appropriate GC flag (`-dead_strip` Mach-O / `--gc-sections` ELF) so an
> unreferenced archive member (tool.o's `hull_tool`) is stripped rather than
> left dangling - Apple ld64 / gnu ld do this by default, zig's lld does not.
> Costs: ~45 MB tool; multi-file tree (binary + `lib/`), so `hull tools install
> zig` extracts a directory, not one binary; pre-1.0 (version-pin it).

## Axis 3 - cross-compilation

Producing a **runnable** app for a target that differs from the host needs
three things aligned to `(os, arch)`, not one:

1. **The emitted object** - `app_registry.o` in the target's format/arch.
   `obj_emit.c` already does this (`tool.emit_app_registry(entries, fmt,
   arch)`); `hull build --target=<arch>` partially wires it (today used for
   AOT). Extend it to also drive `fmt` + the bundled `app_main.o` selection.
2. **A cross-linker** - Axis 1 (lld/mold/zig): `ld.lld` links any ELF target
   regardless of host; `zig cc` cross-links any target.
3. **The target `libhull_platform.a`** - this is the piece often missed:
   the platform library is **compiled per (os, arch)**, so cross-building a
   Linux-aarch64 app from a macOS-arm64 host needs the **Linux-aarch64**
   platform lib, not the host's. That is a `hull platform install <target>`
   fetch (the machinery already exists for flavored/feature libs via
   `hl_release_io_fetch_verified_manifest`), plus the matching bundled
   `app_main.o` + `app_feature_registry-<rt>.o` for that target.

So "cross-compile" = emit-for-target (done) + cross-link (Axis 1/2) +
fetch-target-platform-lib (new). The design should make `--target=<os>-<arch>`
(not just an arch) the single knob that selects all three.

## Rollout (tiers, de-risked)

1. **Done. Tier A + `--linker=`.** `linker_lld.c` via `cc -fuse-ld=lld`,
   `--linker=system|lld|<path>` in `hl_linker_select`, `tool.linker` exposure,
   `e2e_linker.sh` + `e2e_cross_build.sh` (cross-emit + cross-link proof).
   (`hull tools install lld` release asset + `linker_mold.c` deferred; today
   `--linker=lld` resolves lld from `~/.hull/tools` / PATH.)
2. **Tier B, static-musl ELF: DONE and validated end to end.**
   `hl_linker_lld_direct_new` + `--linker=lld-static` invoke `ld.lld -static`
   DIRECTLY against the musl floor (`crt1.o crti.o <objs> --start-group <libs>
   libgcc.a -lc --end-group -L<dir> crtn.o`, floor from `HULL_LIBC_DIR` /
   `/usr/lib`) - **no cc drives the link**. A real Hull app builds to a fully
   static musl binary that runs (`tests/e2e_musl.sh` on Alpine: build hull on
   musl, then `--linker=lld-static` a real app + assert static + run). Making it
   work with real apps took three fixes beyond the toy-stub mechanism:
   (a) admit `ld.lld` in the tool-spawn allowlist (`cap/tool.c`; the bare `ld`
   prefix rejects the `.lld` suffix); (b) the direct linker translates
   build.lua's compiler-driver flags (`-Wl,--whole-archive`, `-Wl,--start-group`,
   ...) to raw linker args and flattens the nested groups into one outer group;
   (c) it links the compiler runtime `libgcc.a` (soft-float builtins like
   `__multf3` a cc driver auto-adds), discovered via `cc -print-libgcc-file-name`
   with a `HULL_LIBGCC` override. The prerequisite "Hull builds on musl" is also
   done (one `-D__O_TMPFILE` shim; see docs/musl_build.md).
   **Honest caveat:** Tier B is compiler-DRIVER-free, but locating `libgcc.a`
   still consults `cc` unless `HULL_LIBGCC` is set - so "no compiler in the link"
   is precise, "no compiler present" needs the override. Bundling the floor +
   libgcc (a `hull tools install libc-musl-<arch>` asset) would make it fully
   self-contained.
3. **Cross-compilation.** `--target=<os>-<arch>` drives emit-fmt + cross-link
   + `hull platform install <target>`. The **object-level** half is proven
   (`e2e_cross_build.sh`); the runnable half needs the target platform lib.
4. **`zig cc` spike** (parallel) - evaluate against Tiers B/3 as a possibly
   simpler total answer (bundles cross sysroots; sidesteps the musl-build work).
5. **Mach-O + Windows Tier B**, then cosmo/APE.

## Testing (cross-compilation is a first-class requirement)

- **Unit** (`test_linker`): `hl_linker_select` resolves system / lld / mold /
  explicit path; a mocked backend records the argv it would run per
  `HlLinkTarget` (so cross-target flag construction is asserted without a real
  linker).
- **e2e, native** (`e2e_linker.sh`): install lld, `hull build --linker=lld`
  an app, run it. Both runtimes. Mirrors `e2e_compiler_free.sh`.
- **e2e, cross-compilation** (`e2e_cross_build.sh`) - the explicit ask:
  - **macOS host -> Linux target**: emit ELF + cross-link (Tier B or `zig`),
    then **run the produced binary in Docker/Lima** (Linux) and hit a route.
    The Hull repo already uses Docker (`ubuntu-24.04`) for the DuckDB e2e and
    Lima is available, so the harness exists.
  - **Linux host -> Linux other-arch**: on `ubuntu-24.04` cross-build an
    `aarch64` app, run under `qemu-user` (or on the `ubuntu-24.04-arm`
    runner, cross-build x86-64 and run under qemu).
  - Assert the produced binary is the **right** `file(1)` type for the
    target, exports `hl_app_entries`, and (where an emulator/VM exists)
    **serves HTTP 200** - the same "relocations resolve at runtime" proof the
    compiler-free e2e uses.
- **CI matrix**: the native `e2e_linker.sh` runs on every platform job; the
  cross e2e runs on the Linux jobs (Docker + qemu are Linux-cheap) and the
  macOS job (Docker Desktop / Lima). Each platform validates that it can
  **target the others**, which is the whole point of cross-compilation.

## Non-goals (first cut)

- Embedding lld/mold in `hull` (they are tools, not bundled).
- Cosmo/APE cross-target (its link path is `apelink`-specific).
- Cross-**compiling** app C (there is none - the app is emitted data + a
  bundled trampoline; only the **link** crosses).
