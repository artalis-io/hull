# Compiler-free `hull build` (object emitter + bundled trampoline)

## Goal

Remove the **C compiler** from `hull build` entirely. Today the build
compiles two C files per app — `app_main.c` (a fixed trampoline) and
`app_registry.c` (pure data) — then links them against
`libhull_platform.a`. Neither actually needs a compiler:

- `app_main.c` is app-invariant (`main()` → `hl_app_run()`), so it is
  **pre-compiled once per (format, arch)** and bundled in the `hull`
  binary.
- `app_registry.c` is a `const HlEntry[]` array of file bytes + names,
  i.e. a **data-only object** that we **emit directly** in the target
  object format — no codegen, just serialization.

What remains on the build path is: pick the bundled `app_main.o`, emit
`app_registry.o`, and **link**. The linker stays (see
[docs/build_flavors.md] and the emitter-vs-overlay analysis: the linked
form keeps `.rodata` sealing, normal OS code-signing, and adds no
runtime container-parsing attack surface). This design covers only the
compiler removal; the linker is a separate, pluggable component.

This subsumes and retires the embedded-TinyCC backend
(`compiler_tcc.c`): TCC was only ever compiling these two files, and it
was compile-only (it delegated linking to system `cc`) and disabled on
cosmo/macOS. The emitter replaces it with something smaller, hardened,
reproducible, and portable across all four object formats.

## The ABI contract the emitter must satisfy

Exact, from `include/hull/entry.h` and the consumers in `src/hull/`:

```c
typedef struct {
    const char          *name;   /* offset 0,  8 bytes */
    const unsigned char *data;   /* offset 8,  8 bytes */
    unsigned int         len;    /* offset 16, 4 bytes; pad 20..23 */
} HlEntry;                        /* sizeof = 24, align = 8 */
```

- Export **one strong global**: `hl_app_entries` (type `const HlEntry[]`),
  overriding the **weak** empty default in `app_entries_default.c`.
- The array is **NULL-name-terminated**: after N real entries, one
  `{ NULL, NULL, 0 }` (24 zero bytes). Every consumer loops on
  `entry.name != NULL`; there is no separate count symbol.
- `name` and `data` are **pointers into the same object** (the name
  strings and file blobs) → each real entry needs **2 absolute-64
  relocations**; `len` is a plain `uint32` immediate.
- All targets are **little-endian** (x86-64, aarch64). No big-endian
  path.

Entry-name conventions (already implemented in `generate_app_registry`,
kept verbatim): Lua modules `./path` (no `.lua`), JS `./path.js`, JSON
`./path.json`, `templates/…`, `static/…`, `migrations/…`, `compute/…`
(`.wasm` + `.aot.<arch>`), `shaders/…`, plus the app signature. The
emitter does **not** decide names — Lua does — it only serializes.

## Architecture

```
              ┌─────────────────────── hull binary ───────────────────────┐
              │  bundled app_main.o  ×  {elf,macho,coff} × {x64,arm64}     │
              │  obj_emit.c  (ELF / Mach-O / COFF backends)                │
              │  HlLinkerVtable (system-ld | lld | mold)                   │
              └────────────────────────────────────────────────────────────┘
                                        │
  app tree ──► generate_app_registry ──►│ list of {entry_name, bytes}
  (Lua, in tool VM: names only)         │
                                        ▼
                            tool.emit_app_registry(target, entries) ──► app_registry.o
                                        │
   pick bundled app_main.o (target) ───┤
   fetch/locate libhull_platform.a ────┤
                                        ▼
                                  [ HlLinkerVtable.link ]
                                        │
                                        ▼
                          app executable (ELF / Mach-O / PE / APE)
```

Data flow, per `hull build`:

1. **Gather** (Lua, unchanged): `generate_app_registry`'s file walk
   produces the ordered `{entry_name, file_bytes}` list. Instead of
   emitting C, it hands the list to the emitter.
2. **Emit** (C): `tool.emit_app_registry(target, entries, out_path)`
   serializes `app_registry.o` in the target's object format.
3. **Select** (Lua): choose the bundled `app_main.o` matching
   `(format, arch)`; extract it to the tmp build dir.
4. **Link** (C vtable): `app_main.o + app_registry.o +
   libhull_platform.a (+ crt + system libs)` → executable. Cosmo path
   feeds two per-arch ELF objects into the cosmo link + `apelink`.

The **compile** step is gone. `HlCompilerVtable` (and `compiler.c`'s `-c` path)
leave the app-data path entirely; the retired `compiler_tcc.c` backend is gone
with it.

## The emitter (`src/hull/obj_emit.c`)

### Format-agnostic core

A single planner lays out the object independent of container format:

- **Blob region**: all file contents concatenated; record each blob's
  offset.
- **String region**: all entry-name strings, NUL-terminated; record
  each name's offset.
- **Array region**: `(N+1)` × 24 bytes. For entry *i*: `len` written
  inline; `name`/`data` slots are **relocation sites** targeting
  `section_base + name_offset` / `section_base + blob_offset`. Sentinel
  = 24 zero bytes, no relocations.

All three regions live in **one read-only section** (`.rodata` /
`__const` / `.rdata`). The planner emits an abstract object:

```
{ section_bytes[],                       // names ++ blobs ++ array
  relocs[] = { site_offset, target_offset },   // 2N entries, all ABS64
  export   = { "hl_app_entries", array_offset, size=(N+1)*24 } }
```

Each backend serializes that abstraction. Ordering is fixed
(names, then blobs, then array) for **reproducibility**; no timestamps.

### Backends (three; cosmo reuses ELF)

| | ELF | Mach-O | COFF/PE |
|---|---|---|---|
| Container | `ET_REL`, `e_machine` = 62 (x64) / 183 (arm64) | `MH_OBJECT`, cputype x86_64 / ARM64 | `IMAGE_FILE_MACHINE_AMD64` / `ARM64` |
| RO section | `.rodata`, `SHT_PROGBITS`, `SHF_ALLOC` (no `SHF_WRITE`) | `__DATA,__const` (or `__TEXT,__const`), `S_REGULAR` | `.rdata`, `CNT_INITIALIZED_DATA|MEM_READ` (no `MEM_WRITE`) |
| Reloc type | `R_X86_64_64` (1) / `R_AARCH64_ABS64` (257) | `X86_64_RELOC_UNSIGNED` / `ARM64_RELOC_UNSIGNED`, len=3 | `IMAGE_REL_AMD64_ADDR64` (1) / `ARM64_ADDR64` |
| Addend | **explicit** in `Elf64_Rela.r_addend` | **in-place** (written into the slot) | **in-place** (written into the slot) |
| Reloc target | local `STT_SECTION` symbol | section-relative (`r_extern=0`, `r_symbolnum`=section) | section symbol index |
| Exported symbol | `hl_app_entries` | **`_hl_app_entries`** (leading `_`) | `hl_app_entries` (x64: no underscore) |
| Long-name handling | `.strtab` | string table | `.rdata`>8ch → COFF string table (`/offset`) |
| Timestamp | none | none | `TimeDateStamp = 0` (reproducibility) |

Three genuine per-format differences to get right:

1. **Symbol naming.** Mach-O prefixes C symbols with `_`
   (`_hl_app_entries`, and the bundled `app_main.o` references
   `_hl_app_run`/`_main`); ELF and Win64-COFF use the bare name.
2. **Addend location.** ELF carries the addend in the relocation
   (`r_addend`); Mach-O and COFF store it **in-place** in the field
   being relocated. The planner therefore also writes the target offset
   into the slot, and the ELF backend additionally zeroes it (addend
   lives in the reloc).
3. **Section/symbol tables.** ELF: `.rodata` + `.rela.rodata` +
   `.symtab` + `.strtab` + `.shstrtab`. Mach-O: one `LC_SEGMENT_64` +
   `LC_SYMTAB`. COFF: section table + symbol table + string table.

### Cosmo / APE

Cosmo objects **are ELF objects**; the APE magic is entirely in the
**link** step (cosmo `ld` + `apelink` combining two per-arch ELF
executables). A data-only object has no code, no TLS, no special
sections, so the ELF backend covers cosmo directly — parameterized by
`(e_machine, EI_OSABI, e_flags)` to match what cosmo's linker expects.
**No separate "APE object" backend is needed.** The two per-arch
`app_registry.o` + `app_main.o` flow into the existing cosmo link path
in `build.lua` (which already handles the `.aarch64/` layout apelink
wants).

## Bundled `app_main.o`

`app_main.c` is invariant:

```c
extern int hl_app_run(int argc, char **argv);
int main(int argc, char **argv) { return hl_app_run(argc, argv); }
```

It contains **code** (a call), so it is compiled once at **Hull release
time** and embedded, per `(format, arch)`:

```
app_main-elf-x86_64.o     app_main-macho-x86_64.o    app_main-coff-x86_64.o
app_main-elf-aarch64.o    app_main-macho-arm64.o
app_main-elf-cosmo-x86_64.o   app_main-elf-cosmo-aarch64.o
```

(~200 bytes each, stable, xxd'd into `build_assets` like the embedded
CA bundle / stdlib.) `hull build` selects by target and
extracts to the tmp dir. Provenance: built by Hull CI, covered by the
release signature.

> Optional future purification: the trampoline is a one-instruction
> tail-call (`jmp/b hl_app_run`), so it could be **emitted** too (a tiny
> code-object backend), removing the pre-built blobs. Bundling is the
> pragmatic v1 — trivial, robust, no hand-written codegen.

### Second bundled object: `app_feature_registry-<rt>.o`

`app_registry.o` is not the only object `hull build` generates today. For
**every** native app, `build.lua` also codegens a tiny C file
(`feature_compose.gen_app_registry_c(rt)`) that defines two functions
overriding the base's weak seams:

```c
extern const HlEntry hl_stdlib_<rt>_entries[];
const HlEntry *const *hl_stdlib_feature_entries(size_t *count)
    { if (count) *count = 1; return (const HlEntry *const[]){ hl_stdlib_<rt>_entries }; }
extern const HlRuntimeFactory hl_<rt>_factory;
const HlRuntimeFactory *const *hl_runtime_feature_factories(size_t *count)
    { if (count) *count = 1; return (const HlRuntimeFactory *const[]){ &hl_<rt>_factory }; }
```

These are **code**, so the data-object emitter can't produce them — but
they are **invariant per runtime** (they depend only on `rt` ∈ {lua, js},
never on the app), so they bundle exactly like `app_main.o`, one blob per
`(rt, format, arch)`:

```
app_feature_registry-lua-macho-arm64.o   app_feature_registry-js-macho-arm64.o
app_feature_registry-lua-elf-x86_64.o    app_feature_registry-js-elf-x86_64.o    …
```

So the compiler-free link set for a plain app is: **emit** `app_registry.o`
+ **bundle-extract** `app_main.o` and `app_feature_registry-<rt>.o` + link.
Three compiles collapse to one emit and two blob extracts.

**Scope of `--no-compiler`.** A `--with=<feature>` build additionally
codegens `feature_registry.c` (filling `hl_db_feature_backends` /
`hl_gpu_feature_backends`), which varies by the feature *combination* and is
genuine code. Bundling every combo is out of scope for v1, so `--no-compiler`
covers the **common case** (no `--with`); a `--with` build still needs the
compiler for that one file (or falls back cleanly). Cosmo's fat base already
has both runtimes' strong hooks compiled in, so a cosmo app emits **no**
`app_feature_registry` at all — only `app_registry.o` + `app_main`.

## Linker integration (`HlLinkerVtable`)

Mirror the existing `HlCompilerVtable`. `include/hull/linker.h`:

```c
typedef struct {
    const char *(*name)(HlLinker*);
    int         (*is_available)(HlLinker*);
    int         (*link)(HlLinker*, const char *out,
                        const char **objs, const char **libs,
                        const HlLinkTarget *tgt);   // format/arch/flavor
    void        (*destroy)(HlLinker*);
} HlLinkerVtable;
```

Backends: `linker_system.c` (invoke `ld`/`cc` where present — the
compiler-free-but-not-linker-free default), and later
`linker_lld.c` / `linker_mold.c` (embedded, extract-and-exec) for a
fully toolchain-free box. Selection parallels `--compiler`:
`--linker=system|lld|mold|<path>`. The linker choice is **orthogonal**
to the emitter — the emitter's output is a standard relocatable object
any of them consume.

Per-target floor the linker still needs (unchanged from any native
build): crt startup + libc/libm/libpthread (ELF), CRT startup + import
descriptors for `kernel32`/`ws2_32`/`ucrtbase` (PE), `libSystem` +
ad-hoc code signature (Mach-O — `ld64.lld` emits the ad-hoc signature),
cosmo crt + `ape.lds` + `apelink` (APE). These are bundle-able but out
of scope here.

## Security invariants (must be preserved)

The whole reason to keep the linked form over an appended overlay:

- **`.rodata` sealing.** The emitted section MUST be read-only
  (ELF: no `SHF_WRITE`; Mach-O: `__const`; COFF: `MEM_READ` without
  `MEM_WRITE`). The linker maps it page-protected read-only, matching
  today's defense against post-boot heap-write tampering of embedded
  modules (Makefile §"Sealed runtime tables"). A writable section is a
  correctness bug in the emitter.
- **Signature coverage.** App bytes are inside the linked image →
  covered normally by Hull's app-layer Ed25519 *and* by any OS code
  signature (no Authenticode/CodeDirectory overlay gap, no
  sign-after-append, no self-exe read, no runtime container parser).
- **No new attack surface.** Unlike an overlay, there is no
  attacker-influenced offset/length table parsed at startup.

## Reproducibility

- Fixed region order (names, blobs, array); deterministic symbol/reloc
  order.
- Zero all timestamp fields (COFF `TimeDateStamp`); ELF/Mach-O objects
  carry none.
- Byte-identical `app_registry.o` for identical inputs → feeds Hull's
  existing reproducible-build gate.

## Testing

- **Unit** (`tests/hull/test_obj_emit.c`): emit for each
  `(format, arch)`; validate structural well-formedness; where the host
  toolchain exists, cross-check with `readelf`/`llvm-objdump` and
  **link + run** a trivial app, asserting the VFS finds every entry and
  the NULL sentinel terminates iteration.
- **Round-trip**: emit → link with system `ld` → run → hit a route that
  reads an embedded template/migration.
- **e2e** (`tests/e2e_compiler_free.sh`, replaces `e2e_tcc.sh`):
  per-format build + serve, on the native runner for each CI matrix
  entry (incl. `ubuntu-24.04-arm`). macOS exercises Mach-O + ad-hoc
  signing; cosmo exercises the dual-arch ELF → apelink path.
- **Hardening**: `checksec`/`readelf` the output to confirm PIE/RELRO
  survive (a linker property, but assert it end-to-end).

## Rollout

1. **Done (#182).** Land `obj_emit.c` + `HlLinkerVtable` +
   `tool.emit_app_registry`/`tool.linker` bindings + `test_obj_emit`.
   **Done (#185).** Bundled `app_main.o` / `app_feature_registry-<rt>.o`
   assets + the `hull build --no-compiler` opt-in path + `e2e_compiler_free.sh`.
2. **Done (Phase 3a).** The emit path is now the **default**. `--compiler[=X]`
   opts back into the C compiler; a `--with` feature (needs a generated
   feature registry), a cosmo/APE target (dual-arch), or a missing linker
   **auto-fall back** to the compiler rather than erroring. The whole e2e
   suite now builds through the emit path on the CI matrix.
3. **Done.** Removed `compiler_tcc.c`, `mk/vendor/tcc.mk`, the `vendor/tcc`
   submodule, `e2e_tcc.sh`, the `test_compiler` tcc cases, the `hull tools
   install tcc` registration, the tcc embedding pipeline, the `HL_ENABLE_TCC`
   flags, and the `build-tcc` release job (this design is its replacement).
   tcc is no longer a Hull-provided, vendored, or installable tool; a user's own
   `tcc` on `$PATH` still works via `--compiler=tcc` as a plain named system
   compiler. The system compiler stays as the `--with`/cosmo fallback.
4. `linker_lld`/`linker_mold` embedding is a follow-up that upgrades
   "compiler-free" to "toolchain-free" on the user's box.

## Open questions / risks

- **Cosmo object acceptance**: confirm cosmo `ld` accepts a plain ELF
  data object with default `EI_OSABI`/`e_flags`, or find the exact
  parameters. Low risk (data-only), verify empirically.
- **COFF specifics**: Win64 no-underscore naming + long-name string
  table (`/offset`) must match what `lld-link`/MSVC `link` expect.
  Verify by linking against a real Windows platform `.a` once it exists.
- **Mach-O ad-hoc signing** rides on the linker (`ld64.lld` does it);
  if a non-lld Mach-O linker is used, a standalone signer is required.
- **app_main.o provenance**: the ~7 bundled objects are release
  artifacts; they must be rebuilt + re-signed when the toolchain or ABI
  changes (rare — the trampoline is frozen).
```
