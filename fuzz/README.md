# Fuzzing

libFuzzer + ASan/UBSan harnesses for Hull's own untrusted-input parsers.
Modelled on `vendor/keel/fuzz` (Keel fuzzes the HTTP / multipart / WebSocket
/ response parsers in its own tree; these cover the parsers that live in
Hull's source, so the two trees don't overlap).

## Harnesses

| Harness | Target | Why |
|---|---|---|
| `fuzz_sh_json` | `sh_json_parse` + accessors + the `a.b[2].c` path-expression parser | Parses untrusted request bodies and config; the recursive walk exercises traversal, not just parsing. |
| `fuzz_path_normalize` | `hl_path_normalize` | Canonicalises `require()`/`import()` paths in place — a `..`-escape or OOB write is a sandbox-traversal primitive. The harness asserts the escape-prevention post-condition. |
| `fuzz_mime_sniff` | `hl_cap_mime_sniff` | Magic-byte + shape sniffing run on the first ~4 KiB of untrusted stored upload bytes; an over-read past `len` while matching a prefix or validating UTF-8 is a crash / info-leak. ASan brackets an exact-sized buffer. |

## Build & run

libFuzzer needs a clang that ships the fuzzer runtime.

```sh
# Linux
make fuzz CC=clang

# macOS (Apple clang lacks libclang_rt.fuzzer_osx.a)
make fuzz CC=/opt/homebrew/opt/llvm@18/bin/clang

# Run a harness over its seed corpus (writes new finds into the corpus dir)
./fuzz/fuzz_sh_json fuzz/corpus_sh_json/ -max_total_time=60
./fuzz/fuzz_path_normalize fuzz/corpus_path_normalize/ -max_total_time=60
./fuzz/fuzz_mime_sniff fuzz/corpus_mime_sniff/ -max_total_time=60

# Or the time-boxed pass CI runs (FUZZ_TIME overrides the per-target seconds):
make fuzz-run CC=clang FUZZ_TIME=60
```

A crashing input is written as `crash-<sha1>` (git-ignored). Reproduce with
`./fuzz/fuzz_sh_json crash-<sha1>`.

## Corpus

`corpus_<target>/` holds a small **curated seed** set checked into git.
libFuzzer-generated entries (hash-named) and crash artifacts are
`.gitignore`d — don't commit them; the seeds are enough to bootstrap and
keep CI deterministic.

## Adding a target

1. Write `fuzz/fuzz_<name>.c` with `int LLVMFuzzerTestOneInput(const uint8_t *, size_t)`.
2. Add a build rule + the target to `fuzz:` in the Makefile (compile the
   parser's sources fresh under `$(FUZZ_CFLAGS)` — these parsers are small
   and self-contained, so no `libhull_platform.a` link).
3. Add `corpus_<name>/` seeds and a CI step.

## What is intentionally *not* fuzzed here

Two parsers that look like targets aren't cleanly fuzzable at the C level,
so they're deliberately left out rather than wrapped in a misleading harness:

- **The manifest extractor** (`manifest_extract_file.c`) spins up a transient
  QuickJS runtime to evaluate `app.manifest({...})` — fuzzing it is fuzzing
  QuickJS, which QuickJS's own corpus already covers.
- **The template compiler** lives in `runtime/{lua,js}/mod_template.c`: the
  parser is implemented *in* Lua/JS, code-generated, and compiled by the
  interpreter. There is no pure-C entry point to feed bytes to.

Other candidates were assessed and skipped on value, not difficulty:
`hl_sig_read` is a thin layer of `sh_json_get` field reads over the
already-fuzzed `sh_json` parser (its hex-decode + Ed25519 path lives in the
separate `hl_sig_verify`); `hl_csp_resolve` and the size-string parser take
operator *config*, not request input, so they're outside the untrusted-input
threat model these harnesses target.
