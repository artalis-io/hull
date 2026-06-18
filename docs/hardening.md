# Hull binary hardening

This document describes the compiler/linker hardening Hull applies to its
release binaries, how each flag was selected, what the per-platform
coverage looks like, and what residual ROP/JOP risk remains.

## Threat model

Hull is a local-first capability-secure runtime. The threat we're
mitigating with hardening is a memory-corruption bug — in Hull itself,
in a vendored library (mbedTLS, QuickJS, Lua, SQLite, miniz, mongoose-
style sh_json/sh_arena, WAMR), or in a developer's app C code linked
against `libhull_platform.a` — being chained into a return-oriented or
jump-oriented programming primitive that subverts capability
enforcement.

The capability layer (the `hl_cap_*` boundary) cannot defend against an
attacker who already controls the instruction pointer. Hardening flags
shrink the window between "bug present" and "exploit landed."

Hull's design also rules out the easy escalation paths:

- **No JIT.** Neither Lua 5.4 nor QuickJS JIT. WAMR is used in
  interpreter mode or AOT (statically compiled ahead of time, never
  written at runtime).
- **No RWX memory.** No mmap/mprotect path takes both `PROT_WRITE` and
  `PROT_EXEC` simultaneously. The sealed-manifest arena (`hl_seal_arena`)
  flips RW → RO via `mprotect` and never the reverse.
- **No writable function-pointer tables.** All dispatch tables
  (`HlRuntimeVtable`, `HlDbBackend`, `HlAsyncBackend`, etc.) live in
  `.rodata` via `const` qualification (see §5 of the C audit skill).

## Flag set, by platform

### Linux (x86_64, aarch64) — release build

Applied unconditionally:

| Flag | Source | Effect |
|---|---|---|
| `-fstack-protector-strong` | CFLAGS baseline | Stack canaries on every function with a stack buffer or `&local` taken |
| `-fPIE` + `-pie` | CFLAGS + LDFLAGS baseline | Position-independent executable → ASLR |
| `-D_FORTIFY_SOURCE=3` | CFLAGS (release only) | Compile-time bounds checks on `memcpy`/`strcpy`/`sprintf`/etc.; runtime `*_chk` variants |
| `-Wl,-z,relro` | LDFLAGS baseline (Linux) | GOT/PLT marked read-only after relocation |
| `-Wl,-z,now` | LDFLAGS baseline (Linux) | Eager bind → combined with relro gives full RELRO |
| `-Wl,-z,noexecstack` | LDFLAGS baseline (Linux) | PT_GNU_STACK without X flag |

Applied if the toolchain accepts (probed by `hl_have_cflag` /
`hl_have_ldflag`):

| Flag | Effect | First supported |
|---|---|---|
| `-fstack-clash-protection` | Probe per stack frame >4K — defeats stack-clash pivots | gcc 8 / clang 11 |
| `-fno-plt` | Direct GOT calls — shrinks ROP gadget surface and lets RELRO+BIND_NOW eliminate every writable function pointer | gcc 7 / clang 5 |
| `-fno-common` | Reject tentative definitions — stricter symbol resolution | default in gcc 10+ / clang 11+; explicit here for older toolchains |
| `-ftrivial-auto-var-init=zero` | Zero-init stack vars — mitigates info-leak primitives | clang 8 / gcc 12 |
| `-fzero-call-used-regs=used-gpr` | Zero scratch GPRs on function return — defeats register-based ROP gadgets that inherit caller register state | clang 15 / gcc 11 |
| `-fcf-protection=full` (x86_64) | Intel CET: ENDBR for IBT + shadow-stack note | gcc 8 / clang 7 |
| `-mbranch-protection=standard` (aarch64) | ARMv8.3 pac-ret + BTI | clang 14 / gcc 9 |
| `-Wl,-z,separate-code` | Separate code/data pages → write primitive on a writable page can't land in executable memory by accident | GNU ld 2.30 / lld |
| `-Wl,--as-needed` | Drop unused DT_NEEDED entries — shrinks loaded-library surface | universal on GNU ld / gold / lld |

### macOS arm64 — release build

Applied unconditionally:

- `-fstack-protector-strong` — canaries.
- `-fPIE` — Mach-O `MH_PIE` flag is the dyld ASLR signal (the `-pie`
  linker flag is unnecessary on Darwin; the compiler default since 10.7
  emits MH_PIE).
- `-D_FORTIFY_SOURCE=3` — `*_chk` variants linked from Apple's libSystem.

Applied if the toolchain accepts (probed):

- `-fno-plt`, `-fno-common`, `-ftrivial-auto-var-init=zero` — all accepted
  by Apple clang.
- `-mbranch-protection=standard` — Apple clang accepts. macOS kernel
  enforcement is partial (PAC is enforced for `arm64e` ABI binaries
  only, which Hull is not), but the instructions are still emitted and
  cost nothing on `arm64`.
- ld64-specific: `-Wl,-z,*` and `-Wl,--as-needed` are **rejected** by
  Apple's linker. The probe correctly excludes them.

### Cosmopolitan / APE — `make CC=cosmocc`

The entire hardening block is skipped (`ifndef COSMO`). APE is a
hybrid MZ/ELF/Mach-O format with its own bootloader; ELF-specific
flags either don't apply (RELRO, BIND_NOW, GNU_STACK) or break the
linker script (PIE, the `-Wl,-z,*` family). What you get:

- **PIE**: not applicable. APE is portable by construction; the
  bootloader maps segments at runtime-determined addresses.
- **Stack canaries**: cosmocc disables `-fstack-protector` by default.
- **NX stack**: handled by the APE bootloader.
- **CET / BTI**: APE binaries must run on pre-CET / pre-BTI CPUs by
  design; emitting the markers would be harmless but the cosmo
  toolchain doesn't enable them.
- **FORTIFY**: not enabled by cosmocc's libc fork.

This is a known and documented trade-off: cosmocc trades hardening for
portability. The hardening summary (`make CC=cosmocc hardening`) prints
each property as `skipped` with the reason.

### Debug / sanitizer builds

- `make debug` — ASan + UBSan, `-O0 -g -fno-omit-frame-pointer`.
  Hardening CFLAGS still applied; FORTIFY suppressed because ASan
  intercepts the `*_chk` variants and the combination is noisy. PIE +
  canaries + CET/BTI still active.
- `make msan` — MSan + UBSan, Linux clang only. Same as above.
- `make coverage` — gcov instrumentation; hardening applied.

### Opt-out

`HULL_DISABLE_HARDENING=1 make` skips the entire block. Use only for
debugging a toolchain interaction; ship of release binaries with this
flag set is gated by `make check-hardening` failing.

## What the audit DID NOT add (and why)

| Considered | Decision | Reason |
|---|---|---|
| `-flto` / `-flto=thin` | **Deferred** | Vendor TUs (mbedtls, sqlite, lua, qjs, miniz) carry their own CFLAGS with `-w` to suppress vendor warnings; mixing LTO across hardened and non-hardened TUs is fragile. Worth a follow-up `HL_ENABLE_LTO=1` build flag once vendor TU LTO compatibility is verified. |
| `-fsanitize=cfi` (LLVM) | **Deferred** | Requires LTO. Indirect-call CFI is the strongest practical ROP mitigation we don't have today; the gating issue is LTO. Same follow-up. |
| `-fsanitize=safe-stack` | **Deferred** | clang-only, splits stacks. Adds runtime cost and is incompatible with `setjmp` / coroutine patterns Hull uses extensively for Lua + JS async. Not free. |
| `-fsanitize=shadow-call-stack` | **Deferred** | aarch64-only. Requires a free register reservation (`-ffixed-x18`). Worth measuring on Linux aarch64 release as a follow-up. |
| `-fhardened` (GCC 14+) | **Skipped** | Meta-flag that conflicts with explicit flag overrides. We deliberately probe individual flags so the build still works on toolchains 5+ years old. |
| Windows CFG | **N/A** | Hull doesn't currently target MSVC / clang-cl. Future Windows support would need its own track. |
| Apply hardening CFLAGS to vendor TUs (mbedtls, sqlite, lua, qjs, miniz, tweetnacl, wamr) | **Deferred** | Vendor TUs use their own `CFLAGS := ... -w ...` arrays that clobber the global set. Adding hardening to all of them is desirable (mbedtls especially) but requires per-vendor verification that the flags don't interact badly with their warning suppression. |
| Apply hardening to `libkeel.a` | **Deferred** | Keel has its own Makefile; the hardening flags would need to be propagated via a Keel-side opt-in or via env-var passthrough. Tracked separately. |
| Strip symbols on release | **Already done** | `release.yml`'s `strip build/wamrc` step handles tools; the main hull binary is stripped by the GH Actions release packaging. |
| Frame-pointer policy | **Unchanged** | Frame pointers stay enabled in DEBUG/COVERAGE (`-fno-omit-frame-pointer`) for profiling. Release uses the compiler default (omit on x86_64, keep on aarch64). Omitting doesn't affect ROP resistance. |

## Verification

### Build-time summary

```
$ make hardening
Hull hardening summary (cc on Linux/x86_64):
  stack canary:     -fstack-protector-strong
  PIE:              -fPIE (linked with -pie)
  fortify:          -D_FORTIFY_SOURCE=3
  probed CFLAGS:     -fstack-clash-protection -fno-plt -fno-common -ftrivial-auto-var-init=zero -fcf-protection=full
  Linux LDFLAGS:    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,-z,separate-code
  link-time:        -Wl,--as-needed
```

### Post-build check

`scripts/check_hardening.sh [BINARY]` (default `build/hull`).

Uses readelf on ELF, otool on Mach-O. Falls back to `skip` per check on
platforms without the relevant tool. Exits non-zero only if a required
protection for THIS platform is missing — never for "not applicable to
this format" properties.

The CI matrix runs `make check-hardening` as a required step on every
Linux + macOS build. A regression that strips hardening fails the
release.

### Properties verified

| Check | ELF | Mach-O | APE |
|---|---|---|---|
| PIE / ASLR | DYN type | MH_PIE flag | skip (APE by construction) |
| RELRO / BIND_NOW | PT_GNU_RELRO + DT_BIND_NOW | skip (dyld) | skip |
| NX stack | PT_GNU_STACK without X | n/a (dyld) | skip |
| Stack canaries | `__stack_chk_fail` referenced | `__stack_chk_fail` referenced | skip |
| FORTIFY | `*_chk` symbols present | skip (Apple naming differs) | skip |
| CET (IBT / SHSTK) | NT_GNU_PROPERTY x86 feature | skip (not in Mach-O) | skip |
| BTI / PAC (arm64) | NT_GNU_PROPERTY AArch64 feature | check (informational) | skip |
| W^X | no LOAD with W+E | no rwx section | skip (APE bootloader) |
| RPATH / RUNPATH | none embedded (informational) | n/a | n/a |
| Hardened Runtime | n/a | `codesign -dv` flag | n/a |

## Residual ROP/JOP risk

What an attacker still gets, even with this hardening landed:

- **Vendor TUs are not yet hardened**. mbedTLS, sqlite, lua, qjs, miniz,
  tweetnacl, wamr each have their own `*_CFLAGS` array in the Makefile
  that clobbers the global set. Gadgets in their text segments are
  reachable.
- **No CFI**. Indirect-call sites in the runtime (Lua C-API, QuickJS
  internals, dispatch vtables) are protected only by `const`
  qualification of the table and by RELRO making the table itself read-
  only. A type-confusion bug that lands a fake vtable pointer is not
  blocked.
- **No shadow stack on Linux x86_64** unless the CPU supports CET and
  the kernel enables `arch_prctl(ARCH_SHSTK_ENABLE)`. Hull emits the
  GNU property note via `-fcf-protection=full`; runtime enforcement is
  the kernel's call.
- **Cosmopolitan binaries have effectively no hardening**. Users who
  ship `hull-cosmo` accept this trade.

## Follow-up roadmap

1. `HL_ENABLE_LTO=1` build flag with vendor TU compatibility testing.
   Unlocks LTO-required mitigations.
2. `-fsanitize=cfi` once LTO is verified.
3. `-fsanitize=shadow-call-stack` measurement on Linux aarch64. If the
   runtime cost is < 2% on the bench suite, ship by default for aarch64.
4. Propagate hardening flags into `vendor/keel/Makefile` and the
   per-vendor `*_CFLAGS` arrays one library at a time, starting with
   mbedTLS (security-critical, well-tested under hardening flags
   elsewhere).
5. Windows Control Flow Guard if/when Hull targets MSVC / clang-cl.

## References

- "Compiler-introduced spurious null-byte writes via `-ftrivial-auto-var-init=zero`": clang docs note (we accept; the alternative `pattern` produces a 0xAAAAAA fill which is also valid).
- Intel CET specification, NT_GNU_PROPERTY ELF note format.
- ARM-A profile ABI for AArch64 BTI/PAC GNU property note.
- `Wl,-z,separate-code` rationale: glibc commit 6c8b1cad8c.
