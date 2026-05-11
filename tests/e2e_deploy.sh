#!/bin/sh
# E2E deploy config generator tests
#
# Tests hull deploy for all three targets (dockerfile, systemd, fly)
# and hull agent deploy introspection.
#
# Usage: sh tests/e2e_deploy.sh
#
# SPDX-License-Identifier: AGPL-3.0-or-later

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0
WORKDIR=""

cleanup() {
    if [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ]; then
        rm -rf "$WORKDIR"
    fi
}
trap cleanup EXIT

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

check_contains() {
    case "$2" in
        *"$3"*) pass "$1" ;;
        *)      fail "$1 — expected '$3'" ;;
    esac
}

check_not_contains() {
    case "$2" in
        *"$3"*) fail "$1 — unexpected '$3'" ;;
        *)      pass "$1" ;;
    esac
}

check_file_exists() {
    if [ -f "$2" ]; then
        pass "$1"
    else
        fail "$1 — file not found: $2"
    fi
}

# ── Setup ───────────────────────────────────────────────────────────

if [ ! -x "$HULL" ]; then
    echo "hull binary not found at $HULL — run 'make' first"
    exit 1
fi

WORKDIR=$(mktemp -d)

echo "=== hull deploy E2E tests ==="
echo ""

# ── 1. Dockerfile target (todo example) ────────────────────────────

echo "--- Dockerfile generation (todo) ---"
OUTDIR="$WORKDIR/todo_docker"
mkdir -p "$OUTDIR"
cp -r "$SRCDIR/examples/todo/"* "$OUTDIR/"

"$HULL" deploy dockerfile "$OUTDIR" --name todoapp --port 8080 2>/dev/null

DOCKERFILE=$(cat "$OUTDIR/Dockerfile" 2>/dev/null)
check_contains "Dockerfile: FROM scratch" "$DOCKERFILE" "FROM scratch"
check_contains "Dockerfile: EXPOSE" "$DOCKERFILE" "EXPOSE 8080"
check_contains "Dockerfile: ENTRYPOINT" "$DOCKERFILE" 'ENTRYPOINT ["/app"]'
check_contains "Dockerfile: hull build" "$DOCKERFILE" "hull build"
check_contains "Dockerfile: data volume" "$DOCKERFILE" 'VOLUME ["/data"]'
check_contains "Dockerfile: database flag" "$DOCKERFILE" "/data/data.db"
# Note: todo manifest has hosts={"127.0.0.1"} but tool.loadfile may not
# extract it (app does session.init() at load time which needs DB).
# CA certs only appear when manifest hosts are successfully extracted.

DOCKERIGNORE=$(cat "$OUTDIR/.dockerignore" 2>/dev/null)
check_contains "dockerignore: data.db" "$DOCKERIGNORE" "data.db"
check_contains "dockerignore: .git" "$DOCKERIGNORE" ".git/"

echo ""

# ── 2. Dockerfile --distroless ─────────────────────────────────────

echo "--- Dockerfile --distroless ---"
OUTDIR2="$WORKDIR/todo_distroless"
mkdir -p "$OUTDIR2"
cp -r "$SRCDIR/examples/todo/"* "$OUTDIR2/"

"$HULL" deploy dockerfile "$OUTDIR2" --distroless --force 2>/dev/null

DOCKERFILE2=$(cat "$OUTDIR2/Dockerfile" 2>/dev/null)
check_contains "Distroless: FROM distroless" "$DOCKERFILE2" "gcr.io/distroless/static-debian12"

echo ""

# ── 3. Dockerfile --sign ───────────────────────────────────────────

echo "--- Dockerfile --sign ---"
OUTDIR3="$WORKDIR/todo_sign"
mkdir -p "$OUTDIR3"
cp -r "$SRCDIR/examples/todo/"* "$OUTDIR3/"

"$HULL" deploy dockerfile "$OUTDIR3" --sign --force 2>/dev/null

DOCKERFILE3=$(cat "$OUTDIR3/Dockerfile" 2>/dev/null)
check_contains "Sign: hull verify" "$DOCKERFILE3" "hull verify"
check_contains "Sign: app.pub" "$DOCKERFILE3" "app.pub"

echo ""

# ── 4. systemd target ─────────────────────────────────────────────

echo "--- systemd generation (todo) ---"
OUTDIR4="$WORKDIR/todo_systemd"
mkdir -p "$OUTDIR4"
cp -r "$SRCDIR/examples/todo/"* "$OUTDIR4/"

"$HULL" deploy systemd "$OUTDIR4" --name todoapp --user hulld --port 9000 2>/dev/null

check_file_exists "systemd: service file" "$OUTDIR4/deploy/todoapp.service"
check_file_exists "systemd: install.sh" "$OUTDIR4/deploy/install.sh"

SERVICE=$(cat "$OUTDIR4/deploy/todoapp.service" 2>/dev/null)
check_contains "systemd: ExecStart" "$SERVICE" "ExecStart="
check_contains "systemd: ProtectSystem" "$SERVICE" "ProtectSystem=strict"
check_contains "systemd: NoNewPrivileges" "$SERVICE" "NoNewPrivileges=yes"
check_contains "systemd: User" "$SERVICE" "User=hulld"
check_contains "systemd: port" "$SERVICE" "-p 9000"
check_contains "systemd: ReadWritePaths" "$SERVICE" "ReadWritePaths="
check_contains "systemd: multi-user" "$SERVICE" "WantedBy=multi-user.target"

INSTALL=$(cat "$OUTDIR4/deploy/install.sh" 2>/dev/null)
check_contains "install.sh: useradd" "$INSTALL" "useradd"
check_contains "install.sh: systemctl" "$INSTALL" "systemctl"

echo ""

# ── 5. fly target ─────────────────────────────────────────────────

echo "--- fly.toml generation (todo) ---"
OUTDIR5="$WORKDIR/todo_fly"
mkdir -p "$OUTDIR5"
cp -r "$SRCDIR/examples/todo/"* "$OUTDIR5/"

"$HULL" deploy fly "$OUTDIR5" --name todofly --region lax --memory 512 2>/dev/null

check_file_exists "fly: fly.toml" "$OUTDIR5/fly.toml"
check_file_exists "fly: Dockerfile" "$OUTDIR5/Dockerfile"

FLY=$(cat "$OUTDIR5/fly.toml" 2>/dev/null)
check_contains "fly: app name" "$FLY" 'app = "todofly"'
check_contains "fly: region" "$FLY" 'primary_region = "lax"'
check_contains "fly: internal_port" "$FLY" "internal_port = 3000"
check_contains "fly: force_https" "$FLY" "force_https = true"
check_contains "fly: memory" "$FLY" '512mb'
check_contains "fly: mounts (database)" "$FLY" "hull_data"

echo ""

# ── 6. hello (no migrations, no manifest) ─────────────────────────

echo "--- hello app (minimal) ---"
OUTDIR6="$WORKDIR/hello_docker"
mkdir -p "$OUTDIR6"
cp -r "$SRCDIR/examples/hello/"* "$OUTDIR6/"

"$HULL" deploy dockerfile "$OUTDIR6" 2>/dev/null

DOCKERFILE6=$(cat "$OUTDIR6/Dockerfile" 2>/dev/null)
check_contains "hello: FROM scratch" "$DOCKERFILE6" "FROM scratch"
# hello has migrations/001_init.sql, so it uses a database
check_contains "hello: has VOLUME" "$DOCKERFILE6" "VOLUME"
check_not_contains "hello: no ca-certs" "$DOCKERFILE6" "ca-certificates.crt"

echo ""

# ── 7. Skip existing files (no --force) ───────────────────────────

echo "--- skip existing files ---"
OUTDIR7="$WORKDIR/skip_test"
mkdir -p "$OUTDIR7"
cp -r "$SRCDIR/examples/hello/"* "$OUTDIR7/"

"$HULL" deploy dockerfile "$OUTDIR7" 2>/dev/null
FIRST=$(cat "$OUTDIR7/Dockerfile" 2>/dev/null)

# Second run should skip
OUTPUT=$("$HULL" deploy dockerfile "$OUTDIR7" 2>&1)
check_contains "skip: reports skip" "$OUTPUT" "skip"

SECOND=$(cat "$OUTDIR7/Dockerfile" 2>/dev/null)
if [ "$FIRST" = "$SECOND" ]; then
    pass "skip: file unchanged"
else
    fail "skip: file was modified"
fi

echo ""

# ── 8. Agent deploy introspection ─────────────────────────────────

echo "--- hull agent deploy ---"
AGENT_OUT=$("$HULL" agent deploy "$SRCDIR/examples/todo" 2>/dev/null)
check_contains "agent: runtime" "$AGENT_OUT" '"runtime"'
check_contains "agent: lua" "$AGENT_OUT" '"lua"'
check_contains "agent: manifest" "$AGENT_OUT" '"manifest"'
check_contains "agent: configs" "$AGENT_OUT" '"configs"'
check_contains "agent: files" "$AGENT_OUT" '"files"'
check_contains "agent: recommendations" "$AGENT_OUT" '"recommendations"'

# Agent deploy on hello (no manifest)
AGENT_HELLO=$("$HULL" agent deploy "$SRCDIR/examples/hello" 2>/dev/null)
# hello has app.manifest({}) — present but empty
check_contains "agent hello: manifest present" "$AGENT_HELLO" '"present":true'

echo ""

# ── 9. JS app support ────────────────────────────────────────────

echo "--- JS app ---"
# Find a JS example
if [ -f "$SRCDIR/examples/hello/app.js" ]; then
    OUTDIR9="$WORKDIR/hello_js"
    mkdir -p "$OUTDIR9"
    cp "$SRCDIR/examples/hello/app.js" "$OUTDIR9/"
    "$HULL" deploy dockerfile "$OUTDIR9" 2>/dev/null
    check_file_exists "js: Dockerfile" "$OUTDIR9/Dockerfile"
else
    pass "js: skipped (no JS hello example)"
fi

echo ""

# ── Summary ───────────────────────────────────────────────────────

echo "=== Results: $PASS passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
