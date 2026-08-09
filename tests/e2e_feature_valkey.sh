#!/bin/sh
# e2e_feature_valkey.sh - Valkey/Redis as a composable feature, end to end.
#
# Builds a BASE hull (EMBED_PLATFORM=1, no Valkey compiled in) + the valkey
# feature archive, then `hull build --with=valkey` an app and runs it against a
# real server. Proves that `hull build` composes the feature lib + a generated
# registry filling the base's weak hl_kv_feature_backends hook (the FIRST non-SQL
# connection feature, its OWN hook - not hl_db_feature_backends) into the app
# binary, so hl_kv_backend_select routes redis:// to the composed backend - while
# the base stays Valkey-free. A plain app must NOT get the feature. Also checks
# that the manifest kv.dynamic allowlist denies a disallowed host BEFORE dialing,
# and that a policy-allowed but unreachable host fails with a CONNECT error (the
# sandbox granted network_outbound), not a scheme/policy rejection.
#
# Must run on a fresh build tree (no prior HL_ENABLE_VALKEY=1 objects), so it
# lives in its own CI job. Uses a local redis-server if present, else docker
# (valkey/valkey:8, falling back to redis:7); SKIPs if neither is available.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

PORT="${VALKEY_TEST_PORT:-16497}"

echo "=== build base hull (EMBED_PLATFORM=1; base is Valkey-free) ==="
make EMBED_PLATFORM=1 >/dev/null
# Stash the base hull: `make feature-valkey` re-invokes make with
# HL_ENABLE_VALKEY=1 and the build-config sentinel cleans build/ on the flag flip
# (EMBED_PLATFORM + VALKEY are both fingerprinted), which would wipe build/hull.
cp build/hull /tmp/hull_base_valkey_e2e

echo "=== base must be Valkey-free ==="
if nm /tmp/hull_base_valkey_e2e 2>/dev/null | grep -q 'hl_kv_backend_valkey'; then
    echo "FAIL: base hull contains the valkey backend"; exit 1
fi

echo "=== build the valkey feature archive ==="
make feature-valkey >/dev/null
ls -la build/libhull_feature-valkey.a

HULL=/tmp/hull_base_valkey_e2e
APP=$(mktemp -d)
PLAIN=$(mktemp -d)
SRV_PID=""; CONTAINER=""
cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null || true
    [ -n "$CONTAINER" ] && docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    rm -rf "$APP" "$PLAIN" /tmp/hull_base_valkey_e2e
}
trap cleanup EXIT INT TERM

# ---- server ----------------------------------------------------------------
wait_ready() {
    i=0; while [ "$i" -lt 50 ]; do
        if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then exec 3>&- 3<&-; return 0; fi
        i=$((i+1)); sleep 0.1
    done
    return 1
}
if command -v redis-server >/dev/null 2>&1; then
    redis-server --port "$PORT" --save '' --appendonly no >/tmp/hull_vk_feat_srv.log 2>&1 &
    SRV_PID=$!; ENGINE="redis-local"
elif command -v docker >/dev/null 2>&1; then
    CONTAINER="hull-valkey-feat-$$"
    if docker pull valkey/valkey:8 >/dev/null 2>&1; then IMG="valkey/valkey:8"; ENGINE="valkey-docker"
    else IMG="redis:7"; ENGINE="redis-docker"; fi
    docker run -d --name "$CONTAINER" -p "$PORT:6379" "$IMG" >/dev/null
else
    echo "SKIP: no redis-server and no docker; cannot run the composed app"; exit 0
fi
wait_ready || { echo "FAIL: $ENGINE not ready on $PORT"; exit 1; }
echo "engine: $ENGINE (port $PORT)"

DSN="redis://127.0.0.1:$PORT"

# ---- compose + run ---------------------------------------------------------
cat > "$APP/app.lua" <<LUA
app.manifest({
    modules = { "hull/kv@1" },
    kv = { dynamic = { schemes = { "redis" }, hosts = { "127.0.0.1" } } },
})
app.main(function(ctx)
    local kv = require("hull.kv").open({ backend = "valkey", dsn = ctx.args[1], namespace = "feat" })
    kv:clear()
    kv:set("k", "v")
    assert(kv:get("k") == "v", "roundtrip failed")
    assert(kv:incr("n", 2) == 2, "incr failed")
    kv:clear(); kv:close()
    print("VALKEY FEATURE APP OK")
    return 0
end)
LUA

echo "=== hull build --with=valkey ==="
BUILD_OUT=$("$HULL" build --compiler=system --with=valkey --no-verify-platform -o "$APP/bin" "$APP" 2>&1) || true
echo "$BUILD_OUT"
echo "$BUILD_OUT" | grep -q "composed feature 'valkey'" || { echo "FAIL: feature not composed"; exit 1; }
"$APP/bin" --no-sandbox -- "$DSN" 2>&1 | grep -q "VALKEY FEATURE APP OK" \
    || { echo "FAIL: composed app did not run"; exit 1; }
echo "ok  --with=valkey composed + ran (--no-sandbox)"

# The composed app connects under the REAL sandbox too (kv.dynamic granted
# network_outbound): a policy-allowed reachable host succeeds.
"$APP/bin" -- "$DSN" 2>&1 | grep -q "VALKEY FEATURE APP OK" \
    || { echo "FAIL: composed app failed under the kernel sandbox"; exit 1; }
echo "ok  ran under the kernel sandbox (network_outbound granted)"

# ---- negative: plain app must NOT compose valkey ---------------------------
echo "=== negative: a plain app must NOT compose valkey ==="
printf 'app.manifest({modules={"hull/kv@1"}})\napp.main(function() print("PLAIN OK") return 0 end)\n' \
    > "$PLAIN/app.lua"
PLAIN_OUT=$("$HULL" build --no-verify-platform -o "$PLAIN/bin" "$PLAIN" 2>&1) || true
if echo "$PLAIN_OUT" | grep -q "composed feature"; then
    echo "$PLAIN_OUT"; echo "FAIL: composed a feature for a plain app"; exit 1
fi
if nm "$PLAIN/bin" 2>/dev/null | grep -q 'hl_kv_backend_valkey'; then
    echo "FAIL: plain app binary contains the valkey backend"; exit 1
fi
echo "ok  plain app is valkey-free"

# ---- negative: manifest kv.dynamic denies a disallowed host ----------------
echo "=== negative: kv.dynamic host allowlist denies before dialing ==="
DENY_OUT=$("$APP/bin" --no-sandbox -- "redis://10.11.12.13:$PORT" 2>&1) || true
echo "$DENY_OUT" | grep -qi "not allowed by kv.dynamic.hosts" \
    || { echo "$DENY_OUT"; echo "FAIL: disallowed host was not denied by policy"; exit 1; }
echo "ok  disallowed host denied by policy (no connect)"

echo "PASS (feature valkey, engine: $ENGINE)"
