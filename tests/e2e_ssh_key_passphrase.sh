#!/bin/sh
# E2E for passphrase-protected OpenSSH private keys.
#
# The oracle is ssh-keygen, not this repository. ssh-keygen WRITES an
# encrypted key, Hull decrypts it, and the public key Hull derives is compared
# against the one `ssh-keygen -y` derives from the same file. A round-trip
# against ourselves would pass just as happily with a wrong KDF, a wrong
# cipher, or a wrong byte order - it would simply be consistently wrong.
#
# That matters more here than usual. A wrong passphrase does not fail in the
# cipher: aes256-ctr is unauthenticated, so the wrong key yields plausible
# garbage. The only integrity signal the format carries is check1 == check2
# inside the plaintext, so "did we get the RIGHT bytes" cannot be answered
# from inside.
#
# Also asserts the refusals, because for a secret the error text is part of
# the contract:
#   - an encrypted key with NO passphrase names passphrase_env in the hint
#   - a WRONG passphrase says so, rather than "the file is corrupt"
#   - an undeclared/unset env var refuses without revealing which it was
#
# Both runtimes: app.lua, and app.js its JS twin through hull:ssh.
#
# Usage: sh tests/e2e_ssh_key_passphrase.sh
# Requires: build/hull, ssh-keygen.
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS_COUNT=0
FAIL_COUNT=0
WORK=""

[ -x "$HULL" ] || { echo "e2e_ssh_key_passphrase: no $HULL - run 'make' first"; exit 1; }
command -v ssh-keygen >/dev/null 2>&1 || {
    echo "e2e_ssh_key_passphrase: ssh-keygen required (openssh-client)"; exit 1; }

cleanup() { [ -n "$WORK" ] && rm -rf "$WORK" || true; }
trap cleanup EXIT INT TERM
WORK=$(mktemp -d 2>/dev/null || mktemp -d -t hullpp)

pass() { echo "  PASS: $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
fail() { echo "  FAIL: $1${2:+ - got: $2}"; FAIL_COUNT=$((FAIL_COUNT + 1)); }

assert_line() {
    a_out="$1"; a_key="$2"; a_want="$3"; a_lbl="$4"
    # A Lua print is a log record ("HH:MM:SS INFO  [app] k=v", audit 29).
    a_got=$(printf '%s\n' "$a_out" | sed -n "s/^\\(.*\\[app\\] \\)\\{0,1\\}${a_key}=\\(.*\\)$/\\2/p" | head -1)
    if [ "$a_got" = "$a_want" ]; then pass "$a_lbl"
    else fail "$a_lbl" "${a_key}=${a_got:-<missing>} (wanted $a_want)"
    fi
}

echo "e2e_ssh_key_passphrase: OpenSSH keys with a passphrase"

PP='correct horse battery staple'

# A passphrase with a space and no shell-safe shape on purpose: it travels
# through an env var and a Lua string, and both must carry it verbatim.
ssh-keygen -q -t ed25519 -N "$PP" -C pp-test -f "$WORK/enc_key" </dev/null
ssh-keygen -q -t ed25519 -N ''   -C plain   -f "$WORK/plain_key" </dev/null

# THE ORACLE: what ssh-keygen itself says this key's public half is.
ssh-keygen -y -P "$PP" -f "$WORK/enc_key" > "$WORK/expected.pub"

cat > "$WORK/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/ssh@1", "hull/fs@1", "hull/env@1" },
    fs = { read = { "enc_key", "plain_key", "expected.pub" } },
    env = { "PP_PASSPHRASE" },
})
local ssh    = require("hull.ssh")
local fs     = require("hull.fs")
local base64 = require("hull.encoding").base64

-- The wire blob base64'd the way an authorized_keys line carries it, so it
-- can be compared against ssh-keygen -y directly.
local function pub_b64(k)
    return base64.encode(k.blob)
end

app.main(function()
    local enc      = fs.read("enc_key")
    local expected = fs.read("expected.pub"):match("^ssh%-ed25519 ([^%s]+)")

    local ok, err = pcall(function() return ssh.load_key(enc) end)
    print("no_pass_refused=" .. tostring(not ok))
    print("no_pass_mentions_env=" .. tostring(
        ok == false and tostring(err):find("passphrase_env", 1, true) ~= nil))

    local ok2, err2 = pcall(function()
        return ssh.load_key(enc, { passphrase = "wrong" })
    end)
    print("wrong_pass_refused=" .. tostring(not ok2))
    print("wrong_pass_named=" .. tostring(
        ok2 == false and tostring(err2):find("wrong passphrase", 1, true) ~= nil))

    local k = ssh.load_key(enc, { passphrase = "correct horse battery staple" })
    print("direct_matches_sshkeygen=" .. tostring(pub_b64(k):find(expected, 1, true) ~= nil))

    local k2 = ssh.load_key(enc, { passphrase_env = "PP_PASSPHRASE" })
    print("env_matches_sshkeygen=" .. tostring(pub_b64(k2):find(expected, 1, true) ~= nil))
    print("env_equals_direct=" .. tostring(k2.blob == k.blob))
    print("secret_is_64=" .. tostring(#k2.secret == 64))

    local k3 = ssh.load_key(fs.read("plain_key"))
    print("plain_still_loads=" .. tostring(k3 ~= nil and #k3.secret == 64))

    local ok6 = pcall(function()
        return ssh.load_key(enc, { passphrase_env = "PP_NOT_DECLARED" })
    end)
    print("undeclared_env_refused=" .. tostring(not ok6))
    return 0
end)
LUA

# The JS twin: the same checks through hull:ssh. A refusal is a thrown Error
# (Lua raises the same message), and the option is spelled passphraseEnv.
cat > "$WORK/app.js" <<'JS'
import { app } from "hull:app";
import { ssh } from "hull:ssh";
import { fs } from "hull:fs";
import { encoding } from "hull:encoding";

app.manifest({
    modules: ["hull/ssh@1", "hull/fs@1", "hull/env@1"],
    fs: { read: ["enc_key", "plain_key", "expected.pub"] },
    env: ["PP_PASSPHRASE"],
});
const text = (ab) => encoding.bytes.fromBuffer(ab);
const pubB64 = (k) => encoding.base64.encode(k.blob);
const refusal = (fn) => { try { fn(); return null; } catch (e) { return String(e && e.message); } };

app.main(() => {
    const enc = fs.read("enc_key");
    const expected = text(fs.read("expected.pub")).match(/^ssh-ed25519 (\S+)/)[1];

    const err = refusal(() => ssh.loadKey(enc));
    console.log("no_pass_refused=" + String(err !== null));
    console.log("no_pass_mentions_env=" + String(err !== null && err.includes("passphraseEnv")));

    const err2 = refusal(() => ssh.loadKey(enc, { passphrase: "wrong" }));
    console.log("wrong_pass_refused=" + String(err2 !== null));
    console.log("wrong_pass_named=" + String(err2 !== null && err2.includes("wrong passphrase")));

    const k = ssh.loadKey(enc, { passphrase: "correct horse battery staple" });
    console.log("direct_matches_sshkeygen=" + String(pubB64(k).includes(expected)));

    const k2 = ssh.loadKey(enc, { passphraseEnv: "PP_PASSPHRASE" });
    console.log("env_matches_sshkeygen=" + String(pubB64(k2).includes(expected)));
    console.log("env_equals_direct=" + String(k2.blob === k.blob));
    console.log("secret_is_64=" + String(k2.secret.length === 64));

    const k3 = ssh.loadKey(fs.read("plain_key"));
    console.log("plain_still_loads=" + String(k3 !== null && k3.secret.length === 64));

    const err6 = refusal(() => ssh.loadKey(enc, { passphraseEnv: "PP_NOT_DECLARED" }));
    console.log("undeclared_env_refused=" + String(err6 !== null));
    return 0;
});
JS

for rt in lua js; do
OUT=$(cd "$WORK" && PP_PASSPHRASE="$PP" "$HULL" --no-sandbox "app.$rt" 2>&1 || true)
env_opt=passphrase_env
[ "$rt" = js ] && env_opt=passphraseEnv

assert_line "$OUT" "no_pass_refused"          "true" "[$rt] an encrypted key with no passphrase is refused"
assert_line "$OUT" "no_pass_mentions_env"     "true" "[$rt] the refusal points at $env_opt"
assert_line "$OUT" "wrong_pass_refused"       "true" "[$rt] a wrong passphrase is refused"
assert_line "$OUT" "wrong_pass_named"         "true" "[$rt] a wrong passphrase says so, not 'corrupt'"
assert_line "$OUT" "direct_matches_sshkeygen" "true" "[$rt] decrypts to the key ssh-keygen -y reports"
assert_line "$OUT" "env_matches_sshkeygen"    "true" "[$rt] the env-sourced passphrase reaches the same key"
assert_line "$OUT" "env_equals_direct"        "true" "[$rt] both routes produce an identical key"
assert_line "$OUT" "secret_is_64"             "true" "[$rt] the secret half is the 64 bytes signing wants"
assert_line "$OUT" "plain_still_loads"        "true" "[$rt] an unencrypted key still loads with no opts"
assert_line "$OUT" "undeclared_env_refused"   "true" "[$rt] an undeclared env var is refused"
if [ "$FAIL_COUNT" -ne 0 ]; then
    echo "  --- app output ---"
    printf '%s\n' "$OUT" | sed 's/^/    /'
fi
done

echo ""
echo "e2e_ssh_key_passphrase: $PASS_COUNT passed, $FAIL_COUNT failed"
[ "$FAIL_COUNT" -eq 0 ] || exit 1
