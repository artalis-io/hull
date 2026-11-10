// test_ssh_known_hosts.js - Tests for hull:ssh:known_hosts and ssh.fileStore /
// ssh.kvStore (JS twin of test_ssh_known_hosts.lua).
//
// The codec against lines in OpenSSH's own format, and the stores over a fake
// hull:fs / hull:kv, so the file round trip runs without a filesystem.
//
// The facade (hull:ssh) is imported dynamically so the codec half still runs
// and reports if the facade fails to load; a facade case then FAILS (it is
// never skipped).

import { knownHosts as kh } from "hull:ssh:known_hosts";
import { wire } from "hull:ssh:wire";
import { encoding } from "hull:encoding";
import { hostkey } from "hull:ssh:hostkey";
import { bytes } from "hull:ssh:bytes";

const base64 = encoding.base64;

let ssh = null;
let sshLoadError = null;
try {
    ssh = (await import("hull:ssh")).ssh;
} catch (e) {
    sshLoadError = e;
}
function needSsh() {
    if (!ssh) throw new Error("hull:ssh did not load: " + ((sshLoadError && sshLoadError.message) || sshLoadError));
}

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try { await fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + ((e && e.message) || e) + "\n" + ((e && e.stack) || "")); }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}

// Lua nil: a JS store may answer null or undefined for "none".
const nil = (v) => (v === undefined ? null : v);

// Lua's `local ok, err = ssh.accept_host(...)`: the JS facade throws a coded
// Error instead of returning (nil, err). -> [value, null] | [null, err]
async function call(fn) {
    try { return [await fn(), null]; } catch (e) { return [null, e]; }
}

// A wire-format key blob of the given type.
const blob = (keytype, byte) => wire.writer().string(keytype).string((byte || "\x01").repeat(32)).build();

const ED_A = blob("ssh-ed25519", "\x01"), ED_B = blob("ssh-ed25519", "\x02");
const RSA = blob("ssh-rsa", "\x03");

const line = (hosts, b) => hosts + " " + kh.blobType(b) + " " + base64.encode(b);
// Lua's `line(...):match(" (.*)")`: everything after the first space.
const afterSpace = (s) => s.slice(s.indexOf(" ") + 1);

// A deterministic stand-in for HMAC-SHA1: 20 bytes depending on key and msg.
function fakeHmac(key, msg) {
    const s = key + "|" + msg;
    let out = "";
    for (let i = 1; i <= 20; i++) {
        let acc = i;
        for (let j = 0; j < s.length; j++) acc = (acc * 31 + s.charCodeAt(j) + i) % 251;
        out += String.fromCharCode(acc);
    }
    return out;
}

// codec -------------------------------------------------------------------------

await test("a plain line lists its names, lower-cased", () => {
    const e = kh.parseLine(line("Web1,[web1]:2222,10.0.0.7", ED_A) + " a comment");
    assertEq(e.names.length, 3);
    assertEq(e.names[0], "web1");
    assertEq(e.names[1], "[web1]:2222");
    assertEq(e.keytype, "ssh-ed25519");
    assertEq(e.blob, ED_A);
    assertEq(kh.matches(e, "[web1]:2222", fakeHmac), true);
    assertEq(kh.matches(e, "web2", fakeHmac), false);
});

await test("comments, markers, blanks and malformed lines are skipped", () => {
    for (const l of ["", "   ", "# a comment",
                     "@cert-authority *.example.com " + afterSpace(line("x", ED_A)),
                     "@revoked " + line("web1", ED_A),
                     "web1 ssh-ed25519 !!!not-base64!!!",
                     "web1 ssh-ed25519",
                     // the blob says rsa, the line says ed25519
                     "web1 ssh-ed25519 " + base64.encode(RSA)]) {
        assertEq(kh.parseLine(l), null, l);
    }
});

await test("patterns are skipped, so a host covered only by one is unknown", () => {
    assertEq(kh.parseLine(line("*.example.com", ED_A)), null);
    const e = kh.parseLine(line("web1,*.internal,!bastion", ED_A));
    assertEq(e.names.length, 1);
    assertEq(kh.matches(e, "db.internal", fakeHmac), false);
});

await test("a hashed name matches only the name it hashes", () => {
    const salt = "s".repeat(20);
    const l = kh.render("web1", ED_A, salt, fakeHmac);
    assertEq(l.slice(0, 3), "|1|");
    const e = kh.parseLine(l);
    assertEq(e.names, undefined);
    assertEq(kh.matches(e, "web1", fakeHmac), true);
    assertEq(kh.matches(e, "web2", fakeHmac), false);
});

await test("a plain line renders the way OpenSSH writes it", () => {
    const l = kh.render("[web1]:2222", ED_A);
    assertEq(l, "[web1]:2222 ssh-ed25519 " + base64.encode(ED_A));
    assertEq(kh.parseLine(l).blob, ED_A);
});

await test("without drops a name, keeps its neighbours and every other line", () => {
    const text = "# keep me\n"
        + line("web1,web2", ED_A) + "\n"
        + line("web3", ED_B) + "\n"
        + kh.render("web1", ED_B, "s".repeat(20), fakeHmac) + "\n";
    const out = kh.without(text, "web1", fakeHmac);
    assertEq(out, "# keep me\n" + line("web2", ED_A) + "\n" + line("web3", ED_B) + "\n");
});

// file_store ---------------------------------------------------------------------
//
// A fake with hull:fs's JS shape: read returns an ArrayBuffer or throws
// an Error "fs.read: <code>" with an own `code`; write throws on refusal.

function fsError(op, code) {
    const e = new Error("fs." + op + ": " + code);
    e.code = code;
    return e;
}

function fakeFs(initial) {
    const fs = { files: { kh: initial }, writes: 0 };
    fs.read = (p) => {
        if (fs.failRead) throw fsError("read", "io_error");
        if (fs.misleadingRead) {
            // The message ends like a missing file, the code says otherwise.
            const e = new Error("fs.read: not_found");
            e.code = "permission_denied";
            throw e;
        }
        if (fs.files[p] === undefined || fs.files[p] === null) throw fsError("read", "not_found");
        return bytes.toU8(fs.files[p]).buffer;
    };
    fs.write = (p, text) => {
        if (fs.deny) throw new Error("fs.write: not permitted");
        fs.writes++;
        fs.files[p] = typeof text === "string" ? text : bytes.fromBuf(text);
        return true;
    };
    return fs;
}

const fakeCrypto = {
    hmacSha1: (msg, key) => fakeHmac(key, msg),
    random: (n) => "r".repeat(n),
};

// The crypto under test goes in through hull:_test_hooks ("hull:ssh"):
// a `crypto` option on a public hull:ssh entry point is refused (audit 44).
// A store takes the crypto it is made with, so each store() sets it.
function store(fs, opts) {
    needSsh();
    opts = opts || {};
    opts.fs = fs;
    const hooks = __testHooks("hull:ssh");
    hooks.setCrypto(fakeCrypto);
    try {
        return ssh.fileStore("kh", opts);
    } finally {
        hooks.setCrypto(null);
    }
}

await test("a missing file is an empty store, created by the first accept", async () => {
    const fs = fakeFs(null);
    const st = store(fs);
    assertEq(nil(await st.get("web1")), null);
    assertEq(await ssh.acceptHost(st, "web1", ED_A), true);
    assertEq(fs.files.kh, line("web1", ED_A) + "\n");
    assertEq(await st.get("web1"), ED_A);
});

await test("a key accepted off port 22 is filed as [host]:port", async () => {
    const fs = fakeFs("");
    const st = store(fs);
    await ssh.acceptHost(st, "Web1", ED_A, 2222);
    assertEq(fs.files.kh, line("[web1]:2222", ED_A) + "\n");
    assertEq(nil(await st.get("web1")), null, "port 22 is a different host:");
});

await test("a host with several recorded keys is known by the best-ranked one", async () => {
    const fs = fakeFs(line("web1", RSA) + "\n" + line("web1,web2", ED_B) + "\n");
    const st = store(fs);
    assertEq(await st.get("web1"), ED_B);
    const e = await st.entries();
    assertEq(e.web1, ED_B);
    assertEq(e.web2, ED_B);
});

await test("a host known only by its RSA key is found by it", async () => {
    const fs = fakeFs(line("web3", RSA) + "\n");
    assertEq(await store(fs).get("web3"), RSA);
    assertEq((await store(fs).entries()).web3, RSA);
});

await test("a key type Hull does not verify is not trusted", async () => {
    const fs = fakeFs(line("old1", blob("ssh-dss", "\x04")) + "\n");
    assertEq(nil(await store(fs).get("old1")), null);
    assertEq((await store(fs).entries()).old1, undefined);
});

await test("a file that does not end in a newline is appended to cleanly", async () => {
    const fs = fakeFs(line("web1", ED_A));
    await store(fs).put("web2", ED_B);
    assertEq(fs.files.kh, line("web1", ED_A) + "\n" + line("web2", ED_B) + "\n");
});

await test("hashed mode writes a hashed entry that it can find again", async () => {
    const fs = fakeFs("");
    const st = store(fs, { hash: true });
    await st.put("web1", ED_A);
    assertEq(fs.files.kh.slice(0, 3), "|1|");
    assertEq(fs.files.kh.indexOf("web1"), -1, "the name is not in the file:");
    assertEq(await st.get("web1"), ED_A);
    assertEq(Object.keys(await st.entries()).length, 0, "a hashed name cannot be listed:");
});

await test("forget_host removes the entry and leaves the rest", async () => {
    const fs = fakeFs(line("web1,web2", ED_A) + "\n");
    const st = store(fs);
    assertEq(await ssh.forgetHost(st, "web1"), true);
    assertEq(nil(await st.get("web1")), null);
    assertEq(await st.get("web2"), ED_A);
});

await test("a store that cannot write says store_failed, not a raise", async () => {
    const fs = fakeFs("");
    fs.deny = true;
    const [ok, err] = await call(() => ssh.acceptHost(store(fs), "web1", ED_A));
    assertEq(ok, null);
    assertEq(err.code, "store_failed");
});

await test("js_fs_audit44: a read failure is told by its code, not its message", async () => {
    const fs = fakeFs(line("web1", ED_A) + "\n");
    fs.misleadingRead = true;
    const st = store(fs);
    let err = null;
    try { await st.get("web1"); } catch (e) { err = e; }
    assertEq(err && err.code, "store_failed");
    assertEq(fs.writes, 0);
});

await test("a store that cannot READ says store_failed and writes nothing", async () => {
    const fs = fakeFs(line("web2", ED_B) + "\n");
    fs.failRead = true;
    const [ok, err] = await call(() => ssh.acceptHost(store(fs), "web1", ED_A));
    assertEq(ok, null);
    assertEq(err.code, "store_failed");
    assertEq(fs.writes, 0);
    assertEq(fs.files.kh, line("web2", ED_B) + "\n");
});

await test("accept_host refuses a blob that is not an ed25519 host key", async () => {
    const fs = fakeFs("");
    for (const b of [RSA, "junk", 42]) {
        const [ok, err] = await call(() => ssh.acceptHost(store(fs), "web1", b));
        assertEq(ok, null);
        assertEq(err.code, "host_key_invalid");
    }
    assertEq(fs.writes, 0);
});

// @revoked ---------------------------------------------------------------------------

await test("revoked_blob reads the key of a @revoked line, and only that", () => {
    assertEq(kh.revokedBlob("@revoked web1 " + afterSpace(line("x", ED_A))), ED_A);
    assertEq(kh.revokedBlob("  @revoked * " + afterSpace(line("x", ED_A)) + " c"), ED_A);
    assertEq(kh.revokedBlob(line("web1", ED_A)), null);
    assertEq(kh.revokedBlob("@cert-authority * " + afterSpace(line("x", ED_A))), null);
    assertEq(kh.revokedBlob("@revoked web1 ssh-ed25519 " + base64.encode(RSA)), null);
    assertEq(kh.revokedBlob("@revoked web1 ssh-ed25519 !!!"), null);
});

await test("a revoked key is refused even where a plain line trusts it", async () => {
    const fs = fakeFs(line("web1", ED_A) + "\n"
        + "@revoked * " + afterSpace(line("x", ED_A)) + "\n");
    const st = store(fs);
    assertEq(await st.revoked(ED_A), true);
    assertEq(await st.revoked(ED_B), false);
    assertEq((await hostkey.check(st, "web1", ED_A))[0], hostkey.REVOKED);
    assertEq((await hostkey.check(st, "web9", ED_A))[0], hostkey.REVOKED);
    assertEq((await hostkey.check(st, "web9", ED_B))[0], hostkey.UNKNOWN);
});

await test("accept_host will not trust a revoked key", async () => {
    const fs = fakeFs("@revoked * " + afterSpace(line("x", ED_A)) + "\n");
    const [ok, err] = await call(() => ssh.acceptHost(store(fs), "web1", ED_A));
    assertEq(ok, null);
    assertEq(err.code, "host_revoked");
    assertEq(fs.writes, 0);
});

// kv_store -------------------------------------------------------------------------
//
// Over a stand-in with hull:kv's JS semantics (cas(k, null, v) is
// set-if-absent and returns false when k exists; scan(prefix) lists keys).

function fakeKv(caps) {
    const data = {};
    return {
        data,
        caps: caps || { compare_exchange: true, scan: true },   // hull:kv's JS caps keys
        get(k) { return data[k] === undefined ? null : data[k]; },
        delete(k) { delete data[k]; return true; },
        cas(k, expected, nv) {
            const cur = data[k] === undefined ? null : data[k];
            if (cur !== (expected === undefined ? null : expected)) return false;
            data[k] = nv;
            return true;
        },
        scan(prefix) {
            const out = [];
            for (const k of Object.keys(data)) if (k.slice(0, prefix.length) === prefix) out.push(k);
            return out;
        },
    };
}

await test("kv_store keeps keys under its prefix, by known_hosts name", async () => {
    needSsh();
    const kv = fakeKv();
    const st = ssh.kvStore(kv);
    assertEq(await ssh.acceptHost(st, "Web1", ED_A, 2222), true);
    assertEq(kv.data["hostkey:[web1]:2222"], ED_A);
    assertEq(await st.get("[web1]:2222"), ED_A);
});

await test("kv_store will not overwrite a key, even under a race", async () => {
    needSsh();
    const kv = fakeKv();
    const st = ssh.kvStore(kv);
    await ssh.acceptHost(st, "web1", ED_A);
    let [ok, err] = await call(() => ssh.acceptHost(st, "web1", ED_B));
    assertEq(ok, null);
    assertEq(err.code, "already_trusted");

    // PARITY: Lua swaps racing.get on the store table; a JS store from the
    // facade may be frozen, so the racing store wraps one instead.
    const real = ssh.kvStore(kv);
    const racing = {
        get: () => null,                              // saw nothing...
        put: (n, b) => real.put(n, b),                // ...but lost the write
        forget: (n) => real.forget(n),
        entries: () => real.entries(),
    };
    [ok, err] = await call(() => ssh.acceptHost(racing, "web1", ED_B));
    assertEq(ok, null);
    assertEq(err.code, "already_trusted");
    assertEq(kv.data["hostkey:web1"], ED_A, "the first key stands:");
});

await test("kv_store shares a namespace: entries lists only its own keys", async () => {
    needSsh();
    const kv = fakeKv();
    kv.data["session:abc"] = "not a host key";
    const st = ssh.kvStore(kv);
    await ssh.acceptHost(st, "web1", ED_A);
    await ssh.acceptHost(st, "web2", ED_B);
    const e = await st.entries();
    assertEq(e.web1, ED_A);
    assertEq(e.web2, ED_B);
    assertEq(e["session:abc"], undefined);
    assertEq(await ssh.forgetHost(st, "web1"), true);
    assertEq(nil(await st.get("web1")), null);
    assertEq(kv.data["session:abc"], "not a host key", "untouched:");
});

await test("kv_store refuses a backend without compare-and-swap", () => {
    needSsh();
    let ok = true;
    try { ssh.kvStore(fakeKv({ scan: true })); } catch (e) { ok = false; }
    assertEq(ok, false);
});

await test("kv_store takes its own prefix", async () => {
    needSsh();
    const kv = fakeKv();
    const st = ssh.kvStore(kv, { prefix: "fleet-a/" });
    await ssh.acceptHost(st, "web1", ED_A);
    assertEq(kv.data["fleet-a/web1"], ED_A);
});

await test("ssh.fingerprint takes just the key blob", () => {
    needSsh();
    const key = wire.writer().string("ssh-ed25519").string("A".repeat(32)).build();
    const crypto = { sha256: () => "\x00".repeat(32) };
    const hooks = __testHooks("hull:ssh");
    hooks.setCrypto(crypto);
    let fp;
    try { fp = ssh.fingerprint(key); } finally { hooks.setCrypto(null); }
    assertEq(fp, "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
