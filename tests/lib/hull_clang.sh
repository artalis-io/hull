# hull_clang.sh - locate a clang that can target wasm32, or report there is none.
#
# `hull compute build` shells out to clang through the same lookup as the shared
# compute_build.lua helper. Where no suitable clang exists the developer
# workflow cannot be exercised at all, and a suite that needs it has nothing to
# measure - "hull compute build: clang not found" is a fact about the host, not
# a defect in Hull.
#
# Factored out because this is the THIRD suite to need it. #501 is the argument:
# two copies of a host probe drifted, one was fixed and the other was missed,
# and the missed one silently ran a suite that should have skipped. One copy.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# hull_find_clang -> prints the clang to use and returns 0, or returns 1.
hull_find_clang() {
    for _p in /opt/homebrew/opt/llvm@18/bin/clang \
              /opt/homebrew/opt/llvm/bin/clang \
              /usr/local/opt/llvm@18/bin/clang \
              /usr/local/opt/llvm/bin/clang; do
        if [ -x "$_p" ]; then echo "$_p"; return 0; fi
    done
    if command -v clang >/dev/null 2>&1; then
        if command -v wasm-ld >/dev/null 2>&1 || \
           clang --print-targets 2>/dev/null | grep -q wasm32; then
            echo "clang"
            return 0
        fi
    fi
    return 1
}

# hull_have_clang -> 0 if one is available. For callers that only need the
# verdict and want to keep their own message.
hull_have_clang() {
    hull_find_clang >/dev/null 2>&1
}

# hull_clang_skip_note - the one-line hint, so every suite says the same thing.
hull_clang_skip_note() {
    echo "  (install brew llvm@18 on macOS, apt install clang lld on Linux)"
}
