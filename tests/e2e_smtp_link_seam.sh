#!/usr/bin/env bash
# e2e_smtp_link_seam.sh - the SMTP/HTTP-client feature composition boundary.
#
# The model-2 async SMTP objects (cap/smtp_async + transport/worker/admission/
# submit/in-flight/op/tls) follow the SAME feature gate as cap/smtp.c (the send +
# the audit writer): all live in libhull_feature-http.a, none in the base. A
# regression that leaves any of them base-resident whole-archives them into a
# non-SMTP app whose base then dangles hl_smtp_audit_complete (the audit writer
# lives only in the feature). This test pins BOTH directions:
#
#   1. Compute/non-HTTP app: composes NO real SMTP object and no undefined kl_*.
#   2. SMTP app: composes the whole coherent set - the async layer AND the audit
#      writer resolve.
#
# REVERT PROOF (manual, documented): remove cap_smtp_async.o et al. from
# FEATURE_SMTP_OBJS in mk/features/http.mk (back into the base) and rebuild - the
# compute app below fails to link with `undefined reference to hl_smtp_audit_complete`.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$ROOT/build/hull"
pass() { echo "  ok: $1"; }
fail() { echo "FAIL: $1"; exit 1; }

command -v nm >/dev/null 2>&1 || { echo "PASS: e2e_smtp_link_seam (nm absent; skipped)"; exit 0; }
[ -x "$HULL" ] || { echo "SKIP: build/hull missing (run make first)"; exit 0; }
# hull build needs a platform lib (embedded, or the local fallback build/libhull_platform.a).
[ -f "$ROOT/build/libhull_platform.a" ] || make -C "$ROOT" platform >/dev/null 2>&1 || \
    { echo "SKIP: no platform lib + no system cc to build one"; exit 0; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ── a compute (non-HTTP, non-SMTP) app.main app ────────────────────────
mkdir -p "$WORK/pc"
printf 'app.manifest({ modules = {} })\napp.main(function() return 7 end)\n' > "$WORK/pc/app.lua"
out=$("$HULL" build --no-verify-platform --compiler=system "$WORK/pc" -o "$WORK/pc/app" 2>&1) \
    || { echo "$out"; fail "compute app must build (no dangling SMTP audit reference)"; }
rc=0; "$WORK/pc/app" >/dev/null 2>&1 || rc=$?
[ "$rc" = 7 ] || fail "compute app should exit 7, got $rc"
# No REAL SMTP capability object linked (the send/transport/worker/audit machinery).
real=$(nm "$WORK/pc/app" 2>/dev/null | grep -cE ' [Tt] _?hl_smtp_(execute|audit_complete|format_message|transport|wop|submit|admission|inflight)' || true)
[ "$real" = 0 ] || fail "compute app links real SMTP objects (got $real), boundary broken"
# A fully-linked executable has no undefined symbols; assert zero undefined kl_*.
undef=$(nm "$WORK/pc/app" 2>/dev/null | grep -cE ' U _?kl_' || true)
[ "$undef" = 0 ] || fail "compute app has $undef undefined kl_* symbols"
pass "compute app: zero real SMTP objects, zero undefined kl_*, runs (exit 7)"

# ── an SMTP app (declares hull/smtp + calls smtp.send) ─────────────────
mkdir -p "$WORK/mail"
cat > "$WORK/mail/app.lua" <<'LUA'
local smtp = require("hull.smtp")
app.manifest({ modules = { "hull/smtp@1" }, hosts = { "127.0.0.1" } })
app.main(function()
    local r = smtp.send({ host = "127.0.0.1", port = 2525, from = "a@x",
                          to = "b@y", subject = "s", body = "b" })
    return r.ok and 0 or 3
end)
LUA
out=$("$HULL" build --no-verify-platform --compiler=system "$WORK/mail" -o "$WORK/mail/app" 2>&1) \
    || { echo "$out"; fail "SMTP app must compose the whole SMTP feature and link"; }
a=$(nm "$WORK/mail/app" 2>/dev/null | grep -cE ' [Tt] _?hl_smtp_audit_complete' || true)
s=$(nm "$WORK/mail/app" 2>/dev/null | grep -cE ' [Tt] _?hl_smtp_server_ctx_init' || true)
e=$(nm "$WORK/mail/app" 2>/dev/null | grep -cE ' [Tt] _?hl_smtp_execute' || true)
[ "$a" -ge 1 ] || fail "SMTP app missing the audit writer (hl_smtp_audit_complete)"
[ "$s" -ge 1 ] || fail "SMTP app missing the async server ctx (hl_smtp_server_ctx_init)"
[ "$e" -ge 1 ] || fail "SMTP app missing the transport execute (hl_smtp_execute)"
pass "SMTP app: async SMTP objects + audit writer all resolve"

echo "PASS: e2e_smtp_link_seam"
