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

Good follow-up targets (need more harness scaffolding — they pull in the
runtime): the manifest extractor and the template compiler.
