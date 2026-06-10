#!/bin/sh
# E2E tests for hull.jwt asymmetric verify (RS256 / ES256 / etc.).
#
# Spins up a tiny fixture app in $TMPDIR that exposes a /run-jwt-tests
# endpoint. The endpoint walks through every interesting scenario
# (valid RS256, valid ES256, alg=none rejected, alg outside allowlist
# rejected, kid resolver, etc.) and returns a JSON pass/fail summary.
#
# Vectors are hardcoded - generated once via openssl against
# committed test keypairs (regen procedure in the script comments).
# Hermetic, no network, no openssl at test time.
#
# Usage: sh tests/e2e_jwt_asym.sh
#        RUNTIME=lua sh tests/e2e_jwt_asym.sh
#        RUNTIME=js  sh tests/e2e_jwt_asym.sh
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=$(pwd)/build/hull
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}

if [ ! -x "$HULL" ]; then
    echo "e2e_jwt_asym: hull binary not found at $HULL - run 'make' first"
    exit 1
fi

fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }

# Pre-generated test vectors (regen via tests/scripts/regen_asym_vectors.sh
# if that script exists; otherwise see tests/hull/cap/test_asym.c for the
# openssl recipe). The RSA keypair + EC keypair match those embedded in
# tests/hull/cap/test_asym.c.

RSA_PUB_PEM='-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA3jPgKzei3EKr8622yh3t
hHdPSZORAfkPAURMHC/KR3ugU24zIoCogSF2kAAiajlcJPVqNZfBsj/xMNMCjFby
nlKeuBLkwkCboidElUhDWNmjR3bbziTMdUC/o6Ko0yKOL7MCGEx/T4P+AyQzIAoi
q2Txjh49ShqxyfahfBMalc3YqtkOcDz6Bl28WoVUpQ7LtATQq8WW+72ibLjdh7q+
uehoe4VNF85K59jq14GKS0irHy1kZy+ZeUhxU0JNMA7wK7oQf56ooFvc0yljeaZy
HH2jv19GmmnfWKGdnBLM9NRaohRnRoILDUwl2GUy1mbKfHHDLSwnVTHgxxhgefBV
AQIDAQAB
-----END PUBLIC KEY-----'

EC256_PUB_PEM='-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAElBmaY23JPwdXiZcM+DBLRJigP2Z7
R1zKHY/lpgqs0hXI06ziLFKaf0YngrAqSgtgs5DlZjWzezD3out89u+1Cw==
-----END PUBLIC KEY-----'

JWT_RS256='eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImsxIn0.eyJzdWIiOiJhbGljZSIsImlzcyI6Imh0dHBzOi8vdGVzdC5leGFtcGxlIiwiZXhwIjo0MTAyNDQ0ODAwfQ.1jgKP0Ppr3pyiCgmAHhQPzB159gXcdLd1l_kTmXF23yejHNZx546Y-SGVOib2-zisuSb2mmHgh9N_iAyNOVAl5JmlD9J7xoV8tEY1za7EdZgGUf5eYqOg60hYMC3E61CDs4T5ZHGGimyc4g54NH1Qp2h_8Z1okXqwUfXPWv4lAGlMaUSvaWup49sisu-ZR5yy01SVECVdLRLWTyW04jznpZqcAKmsDABy8ZiaMu3pbLYeEhd1aHfB2poUeoWx6UAsOuEpOJdo693EE66e0V8toW57B_PpYQ6tk2-WW39dn6cKOj_uVkhvk_4OeEY5saDLhMSEjkn0iwLEIBgHGJrVw'

JWT_ES256='eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJib2IiLCJleHAiOjQxMDI0NDQ4MDB9.4GBjRyU0wlWCVQQwRLy5ktXUumb9pDjEo5pNBweYXZjghCRfpmeyAHXAJcy5i1JHwRsaZS_qKQHI5Lsnqk4qgw'

# An alg=none JWT WITH a bogus sig segment (3 parts, so the alg check
# fires before "invalid format"). Tests that none is rejected even
# when the caller carelessly includes it in opts.algs.
JWT_NONE_ALG='eyJhbGciOiJub25lIn0.eyJzdWIiOiJhdHRhY2tlciJ9.XYZ'

# RS256 JWT with a malformed `exp` (string instead of number) over
# the test RSA keypair. Tests that the type check rejects without
# raising on the string-vs-number compare. We use RS256 instead of
# HS256 for this test because the JS HS256 binding has a known
# round-trip limitation for externally-signed tokens (binary HMAC
# output gets UTF-8-inflated through QuickJS string coercion).
JWT_BAD_EXP='eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhbGljZSIsImV4cCI6Im5vdC1hLW51bWJlciJ9.tbOaO3B6alUSqxarN0c8rlYdYfAIOskPFs9PI3de-joBaxw-otlNf-AoJLlxLSwwPAq6ie-kMyepIqTlLWYk-1WkvmrXLPe1uTIJAKZyeEQ2LRMXLXis14vJuUSg_7vj0SwEJG4M66VRUqW-o4-_5NzF3H1t2BzT3AvSBzdYOQdnaxIfpdqmmP1BbAhmWtRzkSC_lZFqekeyhKSqMhScexDX_bP16GYW_pG57_gLD1Y1P05Xk9KF6jSDPk7_lMOgSncIf4aTxUKT_phhuQ3Q18B1e1mQzZZE4doHJKvn60HRLx5T7Q1Pt2N-rzKAshOrC4S3e_SYP2b5Y3vXmS2d6A'

# An RS256 token whose signature has been corrupted (last byte flipped).
JWT_RS256_TAMPERED=$(printf '%s' "$JWT_RS256" | sed 's/Vw$/VX/')

run_runtime() {
    local rt=$1
    local entry=$2
    local tmp=$(mktemp -d)
    local port=$(( 28000 + $$ % 1000 ))

    case "$entry" in
      app.lua) write_lua_fixture "$tmp" ;;
      app.js)  write_js_fixture "$tmp"  ;;
    esac

    "$HULL" -p "$port" "$tmp/$entry" >"$tmp/server.log" 2>&1 &
    local pid=$!
    trap "kill -9 $pid 2>/dev/null; rm -rf $tmp" EXIT
    sleep 1

    if ! curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/health" | grep -q '^2'; then
        # /health may not exist; just try the test endpoint directly
        true
    fi

    local out=$(curl -s "http://127.0.0.1:$port/run-jwt-tests" || true)
    kill -9 "$pid" 2>/dev/null || true
    trap - EXIT

    if [ -z "$out" ]; then
        fail "$rt: no response from /run-jwt-tests"
        cat "$tmp/server.log" | tail -20
        rm -rf "$tmp"
        return
    fi
    # On failure, also dump server log for diagnosis
    case "$out" in
      *=fail*)
          echo "  --- server.log tail ---"
          tail -10 "$tmp/server.log"
          echo "  --- end ---"
          ;;
    esac

    # Each line is `name=ok` or `name=fail`. Parse + tally.
    local local_pass=0 local_fail=0
    while IFS= read -r line; do
        case "$line" in
          *=ok)   pass "$rt $(echo "$line" | sed 's/=ok$//')"; local_pass=$((local_pass + 1)) ;;
          *=fail) fail "$rt $(echo "$line" | sed 's/=fail$//')"; local_fail=$((local_fail + 1)) ;;
        esac
    done <<EOF
$out
EOF
    if [ "$local_pass" -eq 0 ] && [ "$local_fail" -eq 0 ]; then
        fail "$rt: no test results parsed from /run-jwt-tests output"
        echo "  raw: $out"
    fi
    rm -rf "$tmp"
}

write_lua_fixture() {
    local tmp=$1
    cat > "$tmp/app.lua" <<EOF
app.manifest({
    modules = { "hull/http-server@1", "hull/jwt@1", "hull/crypto@1" },
})
local jwt = require("hull.jwt")
local RSA_PUB_PEM = [==[
$RSA_PUB_PEM
]==]
local EC256_PUB_PEM = [==[
$EC256_PUB_PEM
]==]
local JWT_RS256    = "$JWT_RS256"
local JWT_ES256    = "$JWT_ES256"
local JWT_NONE     = "$JWT_NONE_ALG"
local JWT_TAMP     = "$JWT_RS256_TAMPERED"
local JWT_BAD_EXP  = "$JWT_BAD_EXP"

local function check(name, ok) return string.format("%s=%s", name, ok and "ok" or "fail") end

app.get("/run-jwt-tests", function(req, res)
    local out = {}
    local p, e

    -- rs256_ok: valid RS256 with correct allowlist + PEM
    p, e = jwt.verify(JWT_RS256, RSA_PUB_PEM, { algs = {"RS256"} })
    out[#out+1] = check("rs256_ok", p ~= nil and p.sub == "alice")

    -- rs256_default_algs_rejects: without algs (defaults to HS256-only)
    p, e = jwt.verify(JWT_RS256, RSA_PUB_PEM)
    out[#out+1] = check("rs256_default_algs_rejects", p == nil and e and e:match("not in allowed"))

    -- rs256_tampered_rejected
    p, e = jwt.verify(JWT_TAMP, RSA_PUB_PEM, { algs = {"RS256"} })
    out[#out+1] = check("rs256_tampered_rejected", p == nil and e == "invalid signature")

    -- es256_ok
    p, e = jwt.verify(JWT_ES256, EC256_PUB_PEM, { algs = {"ES256"} })
    out[#out+1] = check("es256_ok", p ~= nil and p.sub == "bob")

    -- none_alg_rejected
    p, e = jwt.verify(JWT_NONE, "anything", { algs = {"HS256","RS256","none"} })
    out[#out+1] = check("none_alg_rejected", p == nil and e and e:match("'none' rejected"))

    -- kid_resolver_routes
    local resolver_called_with
    local function resolver(kid, alg)
        resolver_called_with = kid
        if kid == "k1" then return RSA_PUB_PEM end
        return nil
    end
    p, e = jwt.verify(JWT_RS256, resolver, { algs = {"RS256"} })
    out[#out+1] = check("kid_resolver_routes",
        p ~= nil and resolver_called_with == "k1")

    -- hs256_back_compat: signing + verifying with the legacy API still works
    local tok = jwt.sign({ sub = "carol", exp = 3600 }, "shh-its-a-secret")
    p, e = jwt.verify(tok, "shh-its-a-secret")
    out[#out+1] = check("hs256_back_compat",
        p ~= nil and p.sub == "carol")

    -- resolver_not_called_when_alg_rejected: the headline security
    -- property of opts.algs. If a hostile token claims an alg outside
    -- the allowlist, the resolver MUST NOT be invoked - that's our
    -- defense against alg-confusion side effects (e.g. resolver
    -- doing DB lookups by attacker-controlled kid).
    local resolver_called = false
    local function trap_resolver(kid, alg)
        resolver_called = true
        return RSA_PUB_PEM
    end
    p, e = jwt.verify(JWT_RS256, trap_resolver, { algs = {"HS256"} })
    out[#out+1] = check("resolver_not_called_when_alg_rejected",
        p == nil and not resolver_called)

    -- bad_exp_rejected: malformed exp claim returns clean error
    -- instead of raising on the string vs number compare (would 500).
    p, e = jwt.verify(JWT_BAD_EXP, RSA_PUB_PEM, { algs = {"RS256"} })
    out[#out+1] = check("bad_exp_rejected",
        p == nil and e == "invalid exp claim")

    res:html(table.concat(out, "\n"))
end)
EOF
}

write_js_fixture() {
    local tmp=$1
    cat > "$tmp/app.js" <<EOF
import { app } from "hull:app";
import { jwt } from "hull:jwt";

app.manifest({
    modules: ["hull/http-server@1", "hull/jwt@1", "hull/crypto@1"],
});

const RSA_PUB_PEM = \`$RSA_PUB_PEM\`;
const EC256_PUB_PEM = \`$EC256_PUB_PEM\`;
const JWT_RS256 = "$JWT_RS256";
const JWT_ES256 = "$JWT_ES256";
const JWT_NONE  = "$JWT_NONE_ALG";
const JWT_TAMP  = "$JWT_RS256_TAMPERED";

const check = (name, ok) => \`\${name}=\${ok ? "ok" : "fail"}\`;

app.get("/run-jwt-tests", (req, res) => {
    const out = [];
    let p, e;

    [p, e] = jwt.verify(JWT_RS256, RSA_PUB_PEM, { algs: ["RS256"] });
    out.push(check("rs256_ok", !!p && p.sub === "alice"));

    [p, e] = jwt.verify(JWT_RS256, RSA_PUB_PEM);
    out.push(check("rs256_default_algs_rejects",
        p === null && /not in allowed/.test(e || "")));

    [p, e] = jwt.verify(JWT_TAMP, RSA_PUB_PEM, { algs: ["RS256"] });
    out.push(check("rs256_tampered_rejected",
        p === null && e === "invalid signature"));

    [p, e] = jwt.verify(JWT_ES256, EC256_PUB_PEM, { algs: ["ES256"] });
    out.push(check("es256_ok", !!p && p.sub === "bob"));

    [p, e] = jwt.verify(JWT_NONE, "anything",
        { algs: ["HS256", "RS256", "none"] });
    out.push(check("none_alg_rejected",
        p === null && /'none' rejected/.test(e || "")));

    let resolverCalledWith;
    function resolver(kid, alg) {
        resolverCalledWith = kid;
        return kid === "k1" ? RSA_PUB_PEM : null;
    }
    [p, e] = jwt.verify(JWT_RS256, resolver, { algs: ["RS256"] });
    out.push(check("kid_resolver_routes",
        !!p && resolverCalledWith === "k1"));

    const tok = jwt.sign({ sub: "carol", exp: 3600 }, "shh-its-a-secret");
    [p, e] = jwt.verify(tok, "shh-its-a-secret");
    out.push(check("hs256_back_compat", !!p && p.sub === "carol"));

    // resolver_not_called_when_alg_rejected (see Lua fixture comment)
    let resolverCalled = false;
    function trapResolver(kid, alg) {
        resolverCalled = true;
        return RSA_PUB_PEM;
    }
    [p, e] = jwt.verify(JWT_RS256, trapResolver, { algs: ["HS256"] });
    out.push(check("resolver_not_called_when_alg_rejected",
        p === null && !resolverCalled));

    // bad_exp_rejected: malformed exp must reject cleanly, not raise
    [p, e] = jwt.verify("$JWT_BAD_EXP", RSA_PUB_PEM, { algs: ["RS256"] });
    out.push(check("bad_exp_rejected",
        p === null && e === "invalid exp claim"));

    res.html(out.join("\n"));
});
EOF
}

if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "lua" ]; then
    echo "=== E2E: jwt asym verify (lua) ==="
    run_runtime lua app.lua
fi
if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "js" ]; then
    echo "=== E2E: jwt asym verify (js) ==="
    run_runtime js app.js
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
