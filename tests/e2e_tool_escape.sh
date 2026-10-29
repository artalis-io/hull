#!/bin/sh
# E2E: tool commands never print a raw control byte from the tree they read
# (audit 33).
#
# A cloned app controls its file names, its manifest strings and its
# package.sig / hull.sig. Before audit 33 the tool VM's print / tool.stderr /
# tool.stdout wrote them verbatim, so an ESC / OSC sequence in one of them
# rewrote the terminal, set its title or (OSC 52) the clipboard under
# `hull verify`, `hull inspect`, `hull deploy`, `hull modules list`,
# `hull check`, `hull modules analyze`, `hull analyze`, `hull compute build`
# and `hull eject`. The writers now escape every control byte but LF / tab;
# this runs each command against trees full of ESC and asserts that no 0x1b
# byte reaches stdout or stderr - and that the escaped spelling (\x1b) does,
# so the test cannot pass by a command printing nothing.
#
# Usage: sh tests/e2e_tool_escape.sh
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

fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }

if [ ! -x "$HULL" ]; then
    echo "e2e_tool_escape: $HULL not built (run make)"
    exit 1
fi

WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/hull_e2e_escape.XXXXXX")
# Canonical: on macOS $TMPDIR is under /var, a symlink to /private/var.
WORKDIR=$(cd "$WORKDIR" && pwd -P)

ESC=$(printf '\033')
BEL=$(printf '\007')
# An OSC 52 clipboard write and an OSC 0 title set: what an attacker would plant.
OSC52="${ESC}]52;c;cHduZWQ=${BEL}"
OSC0="${ESC}]0;PWNED${BEL}"

# check_out NAME FILE: FILE (the command's stdout + stderr) holds no raw ESC
# and does show the escaped form, so the planted text was printed.
check_out() {
    if LC_ALL=C grep -q "$ESC" "$2"; then
        fail "$1 printed a raw ESC"
        LC_ALL=C sed -n "/$ESC/{p;q;}" "$2" | od -c | head -3
    else
        pass "$1 printed no raw ESC"
    fi
    if grep -qF '\x1b' "$2"; then
        pass "$1 printed the escaped text"
    else
        fail "$1 did not print the planted text (escaped) at all"
        head -5 "$2"
    fi
}

# run NAME DIR CMD...: run in DIR, both streams into one file.
run() {
    name=$1; dir=$2; shift 2
    out="$WORKDIR/out.$(echo "$name" | tr -c 'A-Za-z0-9' '_')"
    (cd "$dir" && "$@") > "$out" 2>&1
    check_out "$name" "$out"
}

echo "=== tool command output escapes control bytes (audit 33) ==="

# ── An app whose manifest strings carry escapes ─────────────────────────
APP="$WORKDIR/app"
mkdir -p "$APP"
cat > "$APP/app.lua" <<EOF
app.manifest({
    env = { "X\\27]52;c;cHduZWQ=\\7" },
    hosts = { "h\\27[2J" },
    modules = { "hull/json@1\\27]0;PWNED\\7" },
})
app.main(function() return 0 end)
EOF
# A source file whose NAME carries an escape, importing an undeclared module.
printf 'local c = require("hull.crypto")\nlocal unused = 1\nreturn c\n' \
    > "$APP/m${OSC0}.lua"

run "hull deploy (manifest env / hosts)" "$APP" \
    "$HULL" deploy dockerfile . -o "$WORKDIR/deploy_out"
run "hull modules list (manifest modules)" "$APP" "$HULL" modules list .
run "hull check (manifest modules)" "$APP" "$HULL" check .
run "hull modules analyze (file name)" "$APP" "$HULL" modules analyze .
run "hull analyze (file name)" "$APP" "$HULL" analyze .
# eject lists the files it copied only when this hull carries a platform
# library (make EMBED_PLATFORM=1); without one it stops first, and then only
# the no-raw-ESC half of the check applies.
(cd "$APP" && "$HULL" eject . -o "$WORKDIR/ejected") > "$WORKDIR/out.eject" 2>&1
if grep -q "cannot find libhull_platform.a" "$WORKDIR/out.eject"; then
    if LC_ALL=C grep -q "$ESC" "$WORKDIR/out.eject"; then
        fail "hull eject (file name) printed a raw ESC"
    else
        pass "hull eject (no platform library embedded) printed no raw ESC"
    fi
else
    check_out "hull eject (file name)" "$WORKDIR/out.eject"
fi

# ── hull compute build: an invalid module name ──────────────────────────
CAPP="$WORKDIR/capp"
mkdir -p "$CAPP/compute/x${OSC0}"
echo 'app.main(function() return 0 end)' > "$CAPP/app.lua"
echo 'int x;' > "$CAPP/compute/x${OSC0}/x${OSC0}.c"
run "hull compute build (module name)" "$CAPP" "$HULL" compute build

# ── hull inspect: every field of a hand-written signature ───────────────
IAPP="$WORKDIR/iapp"
mkdir -p "$IAPP"
echo 'app.main(function() return 0 end)' > "$IAPP/app.lua"
printf '%s' '{"files":{"x\u001b]0;PWNED\u0007":"aa"},"manifest":{"hosts":["h\u001b[2J"]},"signature":"00","public_key":"k\u001b]52;c;cHduZWQ=\u0007"}' \
    > "$IAPP/hull.sig"
run "hull inspect (signature fields)" "$IAPP" "$HULL" inspect .

# ── a plugin's own error quoting the tree: a raw ESC in a hull.sig ──────
EAPP="$WORKDIR/eapp"
mkdir -p "$EAPP"
echo 'app.main(function() return 0 end)' > "$EAPP/app.lua"
printf '%s{' "$ESC" > "$EAPP/hull.sig"
run "hull inspect (json.decode error)" "$EAPP" "$HULL" inspect .

# ── hull verify: a self-signed app with a planted, unsigned file ────────
# The signer is a Hull app: a legacy hull.sig signs {files, manifest}.
SIGNER="$WORKDIR/signer"
VAPP="$WORKDIR/vapp"
mkdir -p "$SIGNER" "$VAPP/static"
cat > "$SIGNER/app.lua" <<'EOF'
app.manifest({ modules = { "hull/crypto@1", "hull/json@1", "hull/encoding@1" } })
local crypto = require("hull.crypto")
local json = require("hull.json")
local hex = require("hull.encoding").hex
app.main(function(ctx)
    local pk, sk = crypto.ed25519_keypair()
    local files = { ["app.lua"] = ctx.args[1] }
    local sig = crypto.ed25519_sign(json.encode({ files = files, manifest = {} }), sk)
    ctx.stdout:write(json.encode({ files = files, manifest = {},
        signature = hex.encode(sig), public_key = hex.encode(pk) }))
    return 0
end)
EOF
echo 'app.main(function() return 0 end)' > "$VAPP/app.lua"
if command -v shasum >/dev/null 2>&1; then
    SHA=$(shasum -a 256 "$VAPP/app.lua" | cut -d' ' -f1)
else
    SHA=$(sha256sum "$VAPP/app.lua" | cut -d' ' -f1)
fi
"$HULL" "$SIGNER/app.lua" -- "$SHA" > "$VAPP/hull.sig" 2>/dev/null
touch "$VAPP/static/z${OSC52}"
if grep -q '"signature"' "$VAPP/hull.sig"; then
    run "hull verify (unsigned file name)" "$VAPP" "$HULL" verify .
else
    fail "could not produce a self-signed hull.sig for the verify leg"
fi

# ── UTF-8 passes through unchanged ──────────────────────────────────────
UAPP="$WORKDIR/uapp"
mkdir -p "$UAPP"
printf 'app.manifest({ modules = { "hull/json@1caf\303\251" } })\napp.main(function() return 0 end)\n' \
    > "$UAPP/app.lua"
(cd "$UAPP" && "$HULL" modules list .) > "$WORKDIR/out.utf8" 2>&1
if grep -q "caf$(printf '\303\251')" "$WORKDIR/out.utf8"; then
    pass "UTF-8 in a manifest string is printed unchanged"
else
    fail "UTF-8 in a manifest string was altered"
    cat "$WORKDIR/out.utf8"
fi

# ── JSON payloads keep their bytes (written raw) ────────────────────────
(cd "$APP" && "$HULL" manifest .) > "$WORKDIR/out.json" 2>&1
if grep -qF '\u001b' "$WORKDIR/out.json" && ! LC_ALL=C grep -q "$ESC" "$WORKDIR/out.json"; then
    pass "hull manifest JSON keeps json.encode's \\u001b escapes"
else
    fail "hull manifest JSON output changed"
    cat "$WORKDIR/out.json"
fi

# ── App and request text printed to stderr (audit 34) ───────────────────
# app.main's error / rejection / non-numeric return, a handler error that
# quotes a remote multipart field, and an app-context load error were
# written with plain fprintf, so their control bytes reached the operator's
# terminal raw.
echo "=== app and request text on stderr is escaped (audit 34) ==="
RAPP="$WORKDIR/rapp"
mkdir -p "$RAPP/lerr" "$RAPP/lret" "$RAPP/jrej" "$RAPP/jret" "$RAPP/jthrow"
printf 'app.main(function() error("boom\\27]52;c;cHduZWQ=\\7\\nFORGED") end)\n' \
    > "$RAPP/lerr/app.lua"
printf 'app.main(function() return "ret\\27]0;PWNED\\7" end)\n' > "$RAPP/lret/app.lua"
printf 'import { app } from "hull:app";\napp.main(async () => { await null; throw new Error("rej\\x1b]52;c;cHduZWQ=\\x07"); });\n' \
    > "$RAPP/jrej/app.js"
printf 'import { app } from "hull:app";\napp.main(() => "ret\\x1b]0;PWNED\\x07");\n' \
    > "$RAPP/jret/app.js"
printf 'import { app } from "hull:app";\napp.main(() => { throw new Error("thr\\x1b]0;PWNED\\x07"); });\n' \
    > "$RAPP/jthrow/app.js"
run "lua app.main error"            "$RAPP/lerr"   "$HULL" app.lua
run "lua app.main non-numeric"      "$RAPP/lret"   "$HULL" app.lua
run "js app.main promise rejected"  "$RAPP/jrej"   "$HULL" app.js
run "js app.main non-numeric"       "$RAPP/jret"   "$HULL" app.js
run "js app.main throw"             "$RAPP/jthrow" "$HULL" app.js

# hull test: a module-resolver error naming the app's own module string.
TAPP="$WORKDIR/tapp"
mkdir -p "$TAPP/tests"
printf 'app.manifest({ modules = { "hull/http-server@1", "hull/nope\\27]0;PWNED\\7@1" } })\napp.get("/", function(_, res) res:text("x") end)\n' \
    > "$TAPP/app.lua"
printf 'test("t", function() end)\n' > "$TAPP/tests/test_app.lua"
run "hull test (module resolver error)" "$TAPP" "$HULL" test .

# A multipart handler error quoting a field of the remote request body.
if command -v curl >/dev/null 2>&1; then
    MAPP="$WORKDIR/mapp"
    mkdir -p "$MAPP"
    cat > "$MAPP/app.js" <<'JSEOF'
import { app } from "hull:app";
app.manifest({ modules: ["hull/http-server@1"] });
app.get("/health", (_req, res) => { res.text("ok"); });
app.post("/up", async (req, res) => {
    for await (const part of req.multipart()) {
        const u8 = new Uint8Array(await part.read());
        let s = "";
        for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
        throw new Error("bad field: " + s);
    }
    res.text("none");
}, { multipart: { maxPartSize: 65536 } });
JSEOF
    MPORT=$((20000 + $$ % 20000))
    "$HULL" --no-sandbox -p "$MPORT" "$MAPP/app.js" > "$WORKDIR/out.mp" 2>&1 &
    MPID=$!
    up=0
    for _ in $(seq 1 50); do
        if curl -s "http://127.0.0.1:$MPORT/health" >/dev/null 2>&1; then up=1; break; fi
        sleep 0.1
    done
    if [ "$up" -eq 1 ]; then
        curl -s -o /dev/null -X POST "http://127.0.0.1:$MPORT/up" \
            -F "v=x${OSC52}y" >/dev/null 2>&1
        sleep 0.3
        kill "$MPID" 2>/dev/null; wait "$MPID" 2>/dev/null
        check_out "multipart handler error (request body)" "$WORKDIR/out.mp"
    else
        kill "$MPID" 2>/dev/null; wait "$MPID" 2>/dev/null
        fail "multipart server did not start"
        head -5 "$WORKDIR/out.mp"
    fi
else
    echo "  SKIP: curl not found (multipart leg)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
