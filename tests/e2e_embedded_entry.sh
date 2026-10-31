#!/bin/sh
# tests/e2e_embedded_entry.sh - a BUILT binary runs only its embedded entry,
# and its app directory is its working directory (audit 40).
#
# With no entry argument a built binary used to pass its embedded entry name
# (app.lua / app.js) through realpath() and take app_dir from the result, and
# the loaders looked the entry up by the resolved file's BASENAME, falling back
# to disk on a miss. So, in the binary's working directory:
#   - `app.lua -> planted/evil.lua` ran evil.lua as the app;
#   - `app.lua -> /other/app.lua` ran the embedded code but moved app_dir (the
#     sandbox grant, data.db, migrations, the fs root) to /other.
#
# Covers both runners (serve_cli.c for an app.main tool, serve.c for an app
# that composes the HTTP server) in both runtimes, plus dev mode (`hull
# app.lua`), which must still load from disk.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

HULL_BIN="${HULL_BIN:-build/hull}"
. "$(dirname "$0")/lib/hull_rc.sh"
hull_rc_init "$HULL_BIN"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

hull_abs=$(cd "$(dirname "${HULL_BIN}")" && pwd)/$(basename "${HULL_BIN}")

can_build() {
    _doc=$("${HULL_BIN}" doctor --json 2>/dev/null || true)
    case "$_doc" in
        *'"build_compiler":null'*) return 1 ;;
        *'"platform_embedded":"none"'*)
            ls "$(dirname "${HULL_BIN}")"/libhull_platform*.a >/dev/null 2>&1 || return 1 ;;
    esac
    return 0
}

# write_app <dir> <runtime: lua|js> <kind: cli|server> <marker text>
# The app prints "<marker> <contents of marker.txt>" and exits 3 (so a server
# app does not stay serving). A server app also has a migration, so the
# default data.db is created in its app directory.
write_app() {
    _d="$1"; _rt="$2"; _kind="$3"; _m="$4"
    mkdir -p "$_d"
    if [ "$_kind" = server ]; then
        mkdir -p "$_d/migrations"
        echo "CREATE TABLE t (x INTEGER);" > "$_d/migrations/001_init.sql"
    fi
    if [ "$_rt" = lua ]; then
        if [ "$_kind" = server ]; then
            _mods='"hull/fs@1", "hull/http-server@1", "hull/db@1"'
        else
            _mods='"hull/fs@1"'
        fi
        cat > "$_d/app.lua" <<EOF
local fs = require("hull.fs")
app.manifest({ modules = { $_mods }, fs = { read = { "marker.txt" } } })
EOF
        [ "$_kind" = server ] && cat >> "$_d/app.lua" <<'EOF'
app.get("/", function(req, res) res:text("ok") end)
EOF
        cat >> "$_d/app.lua" <<EOF
app.main(function(ctx)
    local ok, v = pcall(fs.read, "marker.txt")
    ctx.stdout:write("$_m " .. (ok and v or "NO-MARKER") .. "\n")
    return 3
end)
EOF
    else
        if [ "$_kind" = server ]; then
            _mods='"hull/fs@1", "hull/http-server@1", "hull/db@1"'
        else
            _mods='"hull/fs@1"'
        fi
        cat > "$_d/app.js" <<EOF
import { app } from "hull:app";
import { fs } from "hull:fs";
app.manifest({ modules: [ $_mods ], fs: { read: [ "marker.txt" ] } });
EOF
        [ "$_kind" = server ] && cat >> "$_d/app.js" <<'EOF'
app.get("/", (req, res) => res.text("ok"));
EOF
        cat >> "$_d/app.js" <<EOF
app.main((ctx) => {
    let v = "NO-MARKER";
    try { v = String.fromCharCode(...new Uint8Array(fs.read("marker.txt"))); } catch (e) {}
    ctx.stdout.write("$_m " + v + "\n");
    return 3;
});
EOF
    fi
}

# build_app <dir> -> the built binary's path, or ""
build_app() {
    _d="$1"
    _cc=""
    hull_is_ape "${HULL_BIN}" && _cc="--compiler cosmocc"
    # shellcheck disable=SC2086
    "${HULL_BIN}" build --no-verify-platform --no-aot $_cc -o "${_d}/app" "${_d}" \
        > "${_d}/build.log" 2>&1
    for _b in "${_d}/app" "${_d}/app.com"; do
        [ -f "$_b" ] && { echo "$_b"; return 0; }
    done
    echo ""
}

# run_in <dir> <binary> -> stdout+stderr of the binary run from <dir>
run_in() {
    (cd "$1" && "$2" 2>&1)
}

check_case() {
    rt="$1"; kind="$2"; ext="$3"
    label="${rt} ${kind}"
    echo "--- built ${label} ---"
    src="$ROOT/src_${rt}_${kind}"
    write_app "$src" "$rt" "$kind" "EMBEDDED-ENTRY"
    bin=$(build_app "$src")
    if [ -z "$bin" ]; then
        fail "${label}: hull build failed: $(tail -5 "$src/build.log")"
        return
    fi

    # 1. Plain: a fresh directory holding only the binary.
    run1="$ROOT/run1_${rt}_${kind}"
    mkdir -p "$run1"
    cp "$bin" "$run1/app"
    echo "CWD" > "$run1/marker.txt"
    out=$(run_in "$run1" ./app)
    case "$out" in
        *"EMBEDDED-ENTRY CWD"*) pass "${label}: plain built binary runs its embedded entry" ;;
        *) fail "${label}: plain run (got: $out)" ;;
    esac

    # 2. app.<ext> -> planted/evil.<ext> (another basename): still the
    #    embedded code, never the planted file.
    run2="$ROOT/run2_${rt}_${kind}"
    mkdir -p "$run2/planted"
    cp "$bin" "$run2/app"
    echo "CWD" > "$run2/marker.txt"
    write_app "$run2/planted_src" "$rt" "$kind" "PLANTED-CODE"
    mv "$run2/planted_src/app.${ext}" "$run2/planted/evil.${ext}"
    rm -rf "$run2/planted_src"
    ln -s "planted/evil.${ext}" "$run2/app.${ext}"
    out=$(run_in "$run2" ./app)
    case "$out" in
        *PLANTED-CODE*) fail "${label}: a planted symlinked entry ran (got: $out)" ;;
        *"EMBEDDED-ENTRY CWD"*) pass "${label}: symlinked entry (other basename) ignored" ;;
        *) fail "${label}: symlinked entry run (got: $out)" ;;
    esac

    # 3. app.<ext> -> /other/app.<ext> (same basename): the app directory
    #    stays the working directory (marker.txt and data.db there).
    run3="$ROOT/run3_${rt}_${kind}"
    other="$ROOT/other_${rt}_${kind}"
    mkdir -p "$run3" "$other"
    cp "$bin" "$run3/app"
    echo "CWD" > "$run3/marker.txt"
    echo "OTHER" > "$other/marker.txt"
    write_app "$other" "$rt" "$kind" "PLANTED-CODE"
    ln -s "$other/app.${ext}" "$run3/app.${ext}"
    out=$(run_in "$run3" ./app)
    case "$out" in
        *PLANTED-CODE*) fail "${label}: same-basename symlink ran other code (got: $out)" ;;
        *"EMBEDDED-ENTRY CWD"*) pass "${label}: same-basename symlink keeps app_dir = cwd" ;;
        *) fail "${label}: same-basename symlink run (got: $out)" ;;
    esac
    if [ "$kind" = server ]; then
        if [ -f "$run3/data.db" ] && [ ! -e "$other/data.db" ]; then
            pass "${label}: default data.db created in the working directory"
        else
            fail "${label}: data.db not in the working directory ($(ls "$run3" "$other" | tr '\n' ' '))"
        fi
    fi

    # 4. Dev mode is unchanged: `hull app.<ext>` loads from disk.
    dev="$ROOT/dev_${rt}_${kind}"
    write_app "$dev" "$rt" "$kind" "DEV-DISK"
    echo "DEVDIR" > "$dev/marker.txt"
    out=$(cd "$dev" && "$hull_abs" --no-sandbox "app.${ext}" 2>&1)
    case "$out" in
        *"DEV-DISK DEVDIR"*) pass "${label}: dev mode loads the entry from disk" ;;
        *) fail "${label}: dev mode (got: $out)" ;;
    esac
}

if ! can_build; then
    echo "SKIP: no build compiler or platform library for hull build"
    exit 0
fi

check_case lua cli lua
check_case js cli js
check_case lua server lua
check_case js server js

echo ""
echo "e2e_embedded_entry: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
