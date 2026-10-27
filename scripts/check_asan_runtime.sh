#!/bin/sh
# scripts/check_asan_runtime.sh CC - fail fast when CC's ASan runtime cannot
# start a process, instead of letting `make debug && make test` hang.
#
# Apple clang 17 (Xcode 17's, clang-1700.x) on macOS 26 deadlocks inside its
# own ASan initialisation, before main: InitializeShadowMemory walks the
# memory map, get_dyld_hdr() calls dyld_shared_cache_iterate_text_swift, that
# mallocs (a Block copy), the malloc re-enters AsanInitFromRtl, which spins
# for ever on the init lock it already holds. Every ASan binary does it, a
# hello-world included, so each test binary sat at 0% progress with no output.
# Homebrew LLVM's runtime (22.x) starts fine.
#
# The probe builds a hello-world with -fsanitize=address and runs it under a
# watchdog. Exit 0 when it prints, 1 (with the fix) when it hangs or fails.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u
CC="${1:-cc}"
TIMEOUT=15

dir="$(mktemp -d "${TMPDIR:-/tmp}/hull-asan-probe.XXXXXX")" || exit 1
trap 'rm -f "$dir/p.c" "$dir/p" "$dir/out"; rmdir "$dir" 2>/dev/null' EXIT

printf '#include <stdio.h>\nint main(void) { puts("asan-ok"); return 0; }\n' > "$dir/p.c"
if ! $CC -fsanitize=address -o "$dir/p" "$dir/p.c" 2>"$dir/out"; then
    echo "check_asan_runtime: $CC cannot build with -fsanitize=address:" >&2
    cat "$dir/out" >&2
    exit 1
fi

"$dir/p" >"$dir/out" 2>&1 &
pid=$!
i=0
while kill -0 "$pid" 2>/dev/null && [ "$i" -lt "$TIMEOUT" ]; do
    sleep 1
    i=$((i + 1))
done
if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    cat >&2 <<EOF
check_asan_runtime: an ASan hello-world built by '$CC' did not start within ${TIMEOUT}s.
  $($CC --version 2>/dev/null | head -1)
This is the Apple clang 17 ASan runtime deadlocking in its own init on macOS 26
(every ASan binary hangs before main, so the tests would too). Use LLVM's clang:
  brew install llvm
  make CC="\$(brew --prefix llvm)/bin/clang" debug && make CC="\$(brew --prefix llvm)/bin/clang" test
EOF
    exit 1
fi
wait "$pid"
rc=$?
if [ "$rc" -ne 0 ] || ! grep -q asan-ok "$dir/out"; then
    echo "check_asan_runtime: an ASan hello-world built by '$CC' failed (exit $rc):" >&2
    cat "$dir/out" >&2
    exit 1
fi
exit 0
