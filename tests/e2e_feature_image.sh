#!/bin/sh
# e2e_feature_image.sh — image codecs as a composable feature: the image-less
# base invariant + the needs_image gate (docs/image_feature.md).
#
# Proves:
#   - the base platform lib is IMAGE-LESS (0 stb symbols);
#   - an image-free app skips the image feature (binary has 0 stb symbols) and
#     still runs;
#   - an image app (declares hull/image) composes the codec core + its runtime's
#     image bridge, and image.new actually works in the produced binary;
#   - both runtimes behave symmetrically.
# Locks in the base-flip + gate so they can't silently regress.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

HULL=./build/hull

echo "=== build hull + the image-less platform lib + the image archives ==="
make >/dev/null
make platform >/dev/null
# The base is image-less: a produced image app composes the codec core +
# its runtime's image bridge, so hull build needs them in build/ (found by
# build.lua). The distributed hull embeds them instead.
make feature-image feature-image-lua feature-image-js >/dev/null

# Count DEFINED symbols matching $2 (exclude undefined 'U'/'u' refs). Mach-O
# prefixes an underscore; ELF does not. We assert what a binary DEFINES.
count_syms() { nm "$1" 2>/dev/null | grep -cE " [A-TV-Za-tv-z] _?$2" || true; }

echo "=== the base platform lib is IMAGE-LESS (0 stb symbols) ==="
base_stb=$(count_syms build/libhull_platform.a "stbi_load_from_memory")
echo "libhull_platform.a: stb=$base_stb"
[ "$base_stb" -eq 0 ] || { echo "FAIL: base platform lib still carries stb"; exit 1; }
echo "ok  base platform lib carries no image codec"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ── fixtures ────────────────────────────────────────────────────────────
# Lua: image app + image-free.
mkdir -p "$WORK/lua_image" "$WORK/lua_free"
cat > "$WORK/lua_image/app.lua" <<'LUA'
local image = require("hull.image")
app.manifest({ modules = { "hull/image@1" } })
app.main(function()
    -- create -> encode PNG -> decode round-trip
    local img = image.new(3, 3, "rgba8", string.rep("\255", 36))
    if img:width() ~= 3 or img:height() ~= 3 then return 7 end
    local png = image.encode(img, "png")
    local back = image.decode(png)
    return (back:width() == 3 and back:height() == 3) and 0 or 8
end)
LUA
cat > "$WORK/lua_free/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() return 4 end)
LUA

# JS: image app + image-free.
mkdir -p "$WORK/js_image" "$WORK/js_free"
cat > "$WORK/js_image/app.js" <<'JS'
import { app } from "hull:app";
import { image } from "hull:image";
app.manifest({ modules: ["hull/image@1"] });
app.main(() => {
    const buf = new Uint8Array(36); buf.fill(255);
    const img = image.new(3, 3, "rgba8", buf.buffer);
    if (img.width !== 3 || img.height !== 3) return 7;
    const png = image.encode(img, "png");
    const back = image.decode(png);
    return (back.width === 3 && back.height === 3) ? 0 : 8;
});
JS
cat > "$WORK/js_free/app.js" <<'JS'
import { app } from "hull:app";
app.manifest({ modules: [] });
app.main(() => 5);
JS

build() { "$HULL" build --no-verify-platform --compiler=system -o "$1/bin" "$1" >/tmp/image_build.log 2>&1; }

# ── 1. image app: composes the feature + image.new runs ─────────────────
for rt in lua js; do
    echo "=== $rt image app composes the image feature + image round-trip runs ==="
    build "$WORK/${rt}_image" || { cat /tmp/image_build.log; echo "FAIL: $rt image build"; exit 1; }
    s=$(count_syms "$WORK/${rt}_image/bin" "stbi_load_from_memory")
    [ "$s" -ge 1 ] || { echo "FAIL: $rt image app has no stb ($s)"; exit 1; }
    set +e; "$WORK/${rt}_image/bin" >/dev/null 2>&1; rc=$?; set -e
    [ "$rc" -eq 0 ] || { echo "FAIL: $rt image app exited $rc (image round-trip broken)"; exit 1; }
    echo "ok  $rt image app: composed (stb=$s), round-trip exit 0"
done

# ── 2. image-free app: drops stb, still runs ────────────────────────────
for rt in lua js; do
    echo "=== $rt image-free app links ZERO stb + still runs ==="
    build "$WORK/${rt}_free" || { cat /tmp/image_build.log; echo "FAIL: $rt free build"; exit 1; }
    s=$(count_syms "$WORK/${rt}_free/bin" "stbi_load_from_memory")
    [ "$s" -eq 0 ] || { echo "FAIL: $rt image-free app still carries stb ($s)"; exit 1; }
    exp=4; [ "$rt" = "js" ] && exp=5
    set +e; "$WORK/${rt}_free/bin" >/dev/null 2>&1; rc=$?; set -e
    [ "$rc" -eq "$exp" ] || { echo "FAIL: $rt free app exited $rc (want $exp)"; exit 1; }
    echo "ok  $rt image-free app: 0 stb, ran (exit $rc)"
done

echo ""
echo "=== PASS: image is a composable feature (base image-less, composed on demand) ==="
