#!/bin/sh
# e2e_check_signed.sh - `hull check` and the app's signature (hsctl gap 10),
# for a Lua app and a JS app:
#   - check_unsigned.passes_<rt>: an unsigned app passes, verify skipped and
#     said so;
#   - check_unsigned.require_fails_<rt>: --require-signature fails it, naming
#     the flag, and runs no verify;
#   - check_unsigned.signed_passes_<rt>: built with --sign, verify runs and
#     passes;
#   - check_unsigned.require_signed_passes_<rt>: --require-signature with that
#     signed app passes;
#   - check_unsigned.tampered_fails_<rt>: the source edited after signing fails
#     at verify (with --require-signature too).
# The signed cases need `hull build`: build/libhull_platform.a and the runtime
# feature archives (make platform feature-lua feature-js, then make
# EMBED_PLATFORM=1 - what `make e2e-build` leaves in build/). Without them
# they SKIP, loudly; HULL_REQUIRE_BUILD=1 (CI) turns the skip into a failure.
# The platform is signed with a throwaway key, so verify runs with
# --no-verify-platform (the gethull layer needs the release key).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HULL=${HULL:-$ROOT/build/hull}
case "$HULL" in /*) ;; *) HULL="$ROOT/$HULL" ;; esac
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
HOME="$WORK/home"
mkdir -p "$HOME"
export HOME
fail=0
ok()  { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }
has() { grep -qF -- "$2" "$1"; }
out="$WORK/out.txt"

# check DIR ARGS...: hull check; output in $out, status in $rc
check() {
    d=$1; shift
    rc=0; "$HULL" check "$@" "$d" > "$out" 2>&1 || rc=$?
}

mkapp() {   # mkapp DIR lua|js
    mkdir -p "$1/tests"
    if [ "$2" = lua ]; then
        printf 'app.manifest({})\napp.main(function(ctx) return 0 end)\n' > "$1/app.lua"
        printf 'test("main exits 0", function()\n    test.eq(test.run_main({}).code, 0)\nend)\n' \
            > "$1/tests/test_app.lua"
    else
        printf 'import { app } from "hull:app";\napp.manifest({});\napp.main((ctx) => 0);\n' > "$1/app.js"
        printf 'test("main exits 0", () => {\n    test.eq(test.runMain([]).code, 0);\n});\n' \
            > "$1/tests/test_app.js"
    fi
}

echo "== hull check: unsigned apps"
for rt in lua js; do
    app="$WORK/unsigned_$rt"
    mkapp "$app" "$rt"
    check "$app"
    [ "$rc" -eq 0 ] && has "$out" "verify skipped: $app is unsigned" &&
        has "$out" "all checks passed" && ! has "$out" "running verify" &&
        ok "check_unsigned.passes_$rt: an unsigned app passes, verify skipped" ||
        { bad "check_unsigned.passes_$rt (rc $rc)"; cat "$out"; }
    check "$app" --require-signature
    [ "$rc" -ne 0 ] &&
        has "$out" "verify failed: $app is unsigned and --require-signature was given" &&
        ! has "$out" "verify skipped" && ! has "$out" "all checks passed" &&
        ok "check_unsigned.require_fails_$rt: --require-signature fails an unsigned app" ||
        { bad "check_unsigned.require_fails_$rt (rc $rc)"; cat "$out"; }
done

echo "== hull check: signed apps"
missing=""
for f in libhull_platform.a libhull_feature-lua.a libhull_feature-js.a; do
    [ -f "$ROOT/build/$f" ] || missing="$missing $f"
done
if [ -n "$missing" ]; then
    if [ "${HULL_REQUIRE_BUILD:-0}" = "1" ]; then
        bad "signed cases: build/ lacks$missing (HULL_REQUIRE_BUILD=1)"
    else
        echo "  SKIP: signed cases - build/ lacks$missing (make platform feature-lua feature-js && make EMBED_PLATFORM=1)"
    fi
    [ "$fail" -eq 0 ] && echo "e2e_check_signed: passed (signed cases skipped)" && exit 0
    exit 1
fi

keys="$WORK/keys"
mkdir -p "$keys"
cp "$ROOT/build/libhull_platform.a" "$keys/"
cp "$ROOT/build/platform_canary_hash" "$keys/" 2>/dev/null || true
( cd "$keys" && "$HULL" keygen dev > /dev/null 2>&1 ) || { bad "hull keygen"; exit 1; }
"$HULL" sign-platform --dir "$keys/" "$keys/dev" > "$out" 2>&1 ||
    { bad "hull sign-platform"; cat "$out"; exit 1; }

for rt in lua js; do
    app="$WORK/signed_$rt"
    mkapp "$app" "$rt"
    "$HULL" build --no-verify-platform --sign "$keys/dev.key" \
        --platform-sig "$keys/platform.sig" -o "$app/app" "$app" > "$out" 2>&1 &&
        [ -f "$app/package.sig" ] ||
        { bad "check_unsigned.signed_build_$rt: hull build --sign"; cat "$out"; continue; }
    check "$app" --no-verify-platform
    [ "$rc" -eq 0 ] && has "$out" "running verify" && has "$out" "all checks passed" &&
        ! has "$out" "verify skipped" &&
        ok "check_unsigned.signed_passes_$rt: a signed app is verified and passes" ||
        { bad "check_unsigned.signed_passes_$rt (rc $rc)"; cat "$out"; }
    check "$app" --require-signature --no-verify-platform
    [ "$rc" -eq 0 ] && has "$out" "running verify" && has "$out" "all checks passed" &&
        ok "check_unsigned.require_signed_passes_$rt: --require-signature passes a signed app" ||
        { bad "check_unsigned.require_signed_passes_$rt (rc $rc)"; cat "$out"; }
    if [ "$rt" = lua ]; then c="--"; else c="//"; fi
    echo "$c edited after signing" >> "$app/app.$rt"
    check "$app" --no-verify-platform
    [ "$rc" -ne 0 ] && has "$out" "running verify" && has "$out" "hull verify: FAILED" &&
        ! has "$out" "all checks passed" && ok "check_unsigned.tampered_fails_$rt: a source edited after signing fails at verify" ||
        { bad "check_unsigned.tampered_fails_$rt (rc $rc)"; cat "$out"; }
    check "$app" --require-signature --no-verify-platform
    [ "$rc" -ne 0 ] && has "$out" "hull verify: FAILED" &&
        ! has "$out" "all checks passed" && ok "check_unsigned.require_tampered_fails_$rt: and with --require-signature" ||
        { bad "check_unsigned.require_tampered_fails_$rt (rc $rc)"; cat "$out"; }
done

[ "$fail" -eq 0 ] && echo "e2e_check_signed: all passed" || exit 1
