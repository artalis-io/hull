// test_ssh_hostkey.js - Tests for hull:ssh:hostkey (JS twin of
// test_ssh_hostkey.lua).
//
// The trust-state cases are the point: an unknown host and a CHANGED host must
// never be conflated, because the second is what a man-in-the-middle looks like.

import { hostkey } from "hull:ssh:hostkey";
import { wire } from "hull:ssh:wire";
import { ssh } from "hull:ssh";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try { await fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + ((e && e.message) || e) + "\n" + ((e && e.stack) || "")); }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}

async function assertRaises(fn, msg) {
    let ok = true;
    try { await fn(); } catch (e) { ok = false; }
    if (ok) throw new Error((msg || "should have raised") + " but did not");
}

const KEY_A = "\x01".repeat(32);
const KEY_B = "\x02".repeat(32);

const keyBlob = (key) => wire.writer().string("ssh-ed25519").string(key).build();
const sigBlob = (sig) => wire.writer().string("ssh-ed25519").string(sig).build();

// an in-memory store, the shape an application supplies
function newStore(seed) {
    const t = {};
    for (const k of Object.keys(seed || {})) t[k] = seed[k];
    return {
        get: (host) => t[host],
        put: (host, blob) => { t[host] = blob; },
        forget: (host) => { delete t[host]; },
        _table: t,
    };
}

// blob parsing -------------------------------------------------------------

await test("parse_key reads an ssh-ed25519 blob", () => {
    const k = hostkey.parseKey(keyBlob(KEY_A));
    assertEq(k.algorithm, "ssh-ed25519");
    assertEq(k.key, KEY_A);
});

await test("parse_key refuses another algorithm", async () => {
    const blob = wire.writer().string("ssh-rsa").string(KEY_A).build();
    await assertRaises(() => hostkey.parseKey(blob), "ssh-rsa");
});

await test("parse_key refuses a key of the wrong length", async () => {
    await assertRaises(() => hostkey.parseKey(keyBlob("\x01".repeat(31))), "31 bytes");
    await assertRaises(() => hostkey.parseKey(keyBlob("\x01".repeat(33))), "33 bytes");
});

await test("parse_key refuses a truncated blob", async () => {
    await assertRaises(() => hostkey.parseKey(keyBlob(KEY_A).slice(0, 10)), "truncated");
});

await test("parse_signature reads and length-checks", async () => {
    const s = hostkey.parseSignature(sigBlob("\x09".repeat(64)));
    assertEq(s.signature, "\x09".repeat(64));
    await assertRaises(() => hostkey.parseSignature(sigBlob("\x09".repeat(63))), "63-byte sig");
});

// base64 and fingerprints ------------------------------------------------------

await test("fingerprint has the OpenSSH shape", () => {
    const fakeSha = () => "\x00".repeat(32);
    const fp = hostkey.fingerprint(fakeSha, keyBlob(KEY_A));
    assertEq(fp.slice(0, 7), "SHA256:");
    assertEq(fp.length, 7 + 43);
    assertEq(fp, "SHA256:" + "A".repeat(43));
});

await test("fingerprint needs a hash function", async () => {
    await assertRaises(() => hostkey.fingerprint(null, keyBlob(KEY_A)), "no hash");
});

// trust states ------------------------------------------------------------------

await test("an unseen host is unknown, not trusted", async () => {
    const store = newStore();
    const [status, stored] = await hostkey.check(store, "spark.local", keyBlob(KEY_A));
    assertEq(status, hostkey.UNKNOWN);
    assertEq(stored, null);
});

await test("a matching stored key is trusted", async () => {
    const store = newStore({ "spark.local": keyBlob(KEY_A) });
    const [status] = await hostkey.check(store, "spark.local", keyBlob(KEY_A));
    assertEq(status, hostkey.TRUSTED);
});

await test("a different key for a known host is CHANGED, never unknown", async () => {
    const store = newStore({ "spark.local": keyBlob(KEY_A) });
    const [status, stored] = await hostkey.check(store, "spark.local", keyBlob(KEY_B));
    assertEq(status, hostkey.CHANGED);
    assertEq(stored, keyBlob(KEY_A));
});

await test("trust is per host, not global", async () => {
    const store = newStore({ "a.local": keyBlob(KEY_A) });
    assertEq((await hostkey.check(store, "b.local", keyBlob(KEY_A)))[0], hostkey.UNKNOWN);
});

await test("check demands a real store", async () => {
    await assertRaises(() => hostkey.check(null, "h", keyBlob(KEY_A)), "nil store");
    await assertRaises(() => hostkey.check({}, "h", keyBlob(KEY_A)), "no get");
});

// accepting and forgetting -------------------------------------------------------

await test("a key is stored under host and port, as known_hosts names it", () => {
    assertEq(hostkey.storeName("spark.local", 22), "spark.local");
    assertEq(hostkey.storeName("spark.local"), "spark.local", "port defaults to 22:");
    assertEq(hostkey.storeName("spark.local", 2222), "[spark.local]:2222");
    assertEq(hostkey.storeName("Spark.LOCAL", 22), "spark.local", "case-insensitive:");
});

await test("a key accepted on one port is unknown on another", async () => {
    // PARITY: Lua uses require('hull.ssh').memory_store(); this suite stays
    // independent of hull:ssh (the facade) and uses the same-shaped store.
    const store = newStore();
    await hostkey.acceptNew(store, hostkey.storeName("spark.local", 22), keyBlob(KEY_A));
    assertEq((await hostkey.check(store, hostkey.storeName("spark.local", 2222),
                                  keyBlob(KEY_B)))[0], hostkey.UNKNOWN);
    assertEq((await hostkey.check(store, hostkey.storeName("SPARK.local", 22),
                                  keyBlob(KEY_A)))[0], hostkey.TRUSTED);
});

await test("accept_new remembers a first-use key", async () => {
    const store = newStore();
    await hostkey.acceptNew(store, "spark.local", keyBlob(KEY_A));
    assertEq((await hostkey.check(store, "spark.local", keyBlob(KEY_A)))[0], hostkey.TRUSTED);
});

await test("accept_new refuses to overwrite", async () => {
    const store = newStore({ "spark.local": keyBlob(KEY_A) });
    await assertRaises(() => hostkey.acceptNew(store, "spark.local", keyBlob(KEY_B)), "overwrite");
    assertEq(store.get("spark.local"), keyBlob(KEY_A));
});

await test("forget then accept_new is how a rotated host is re-trusted", async () => {
    const store = newStore({ "spark.local": keyBlob(KEY_A) });
    await hostkey.forget(store, "spark.local");
    assertEq((await hostkey.check(store, "spark.local", keyBlob(KEY_B)))[0], hostkey.UNKNOWN);
    await hostkey.acceptNew(store, "spark.local", keyBlob(KEY_B));
    assertEq((await hostkey.check(store, "spark.local", keyBlob(KEY_B)))[0], hostkey.TRUSTED);
});

// pending trust (platform gap 4) -------------------------------------------

await test("a pending trust accepts the first key presented and is then held to it", async () => {
    const p = ssh.pendingTrust();
    assertEq(p.get("spark.local"), null, "nothing before a key is presented:");
    const [status, stored] = await hostkey.check(p, "spark.local", keyBlob(KEY_A));
    assertEq(status, hostkey.TRUSTED);
    assertEq(stored, keyBlob(KEY_A));
    assertEq(p.get("spark.local"), keyBlob(KEY_A), "the accepted key is readable:");
    assertEq((await hostkey.check(p, "spark.local", keyBlob(KEY_A)))[0], hostkey.TRUSTED);
    const [st2, stored2] = await hostkey.check(p, "spark.local", keyBlob(KEY_B));
    assertEq(st2, hostkey.CHANGED);
    assertEq(stored2, keyBlob(KEY_A));
    assertEq((await hostkey.check(p, "other.local", keyBlob(KEY_A)))[0], hostkey.CHANGED,
             "one pending trust, one host:");
    assertEq(p.put, undefined, "a pending trust persists nothing:");
});

await test("acceptHost refuses a pending trust as the store", async () => {
    const p = ssh.pendingTrust();
    let code = null;
    try { await ssh.acceptHost(p, "spark.local", keyBlob(KEY_A)); } catch (e) { code = e.code; }
    assertEq(code, "not_a_store");
    assertEq(p.get("spark.local"), null, "and recorded nothing:");
});

await test("a pending trust is single use: the second connect is refused before any dial", async () => {
    const p = ssh.pendingTrust();
    const attempt = async (o) => { try { await ssh.connect(o); return null; } catch (e) { return e.code; } };
    assertEq(await attempt({
        host: "spark.local", user: "operator", key: {}, crypto: {}, trust: p,
        openStream: () => [null, "no route", "connect_failed"],
    }), "connect_failed");
    let dialled = false;
    assertEq(await attempt({
        host: "spark.local", user: "operator", key: {}, crypto: {}, trust: p,
        openStream: () => { dialled = true; throw new Error("nothing may be dialled"); },
    }), "already_used");
    assertEq(dialled, false, "refused before the dial:");
    assertEq(await attempt({
        host: "spark.local", user: "operator", key: {}, crypto: {}, trust: ssh.pendingTrust(),
        openStream: () => [null, "no route", "connect_failed"],
    }), "connect_failed");
});

// signature verification -----------------------------------------------------------

const fakeSha = (data) => (data + "\x00".repeat(32)).slice(0, 32);
const cryptoStub = (result) => ({ ed25519Verify: () => result, sha256: fakeSha });

await test("verify_signature passes a good signature through", () => {
    const [ok] = hostkey.verifySignature(cryptoStub(true),
        keyBlob(KEY_A), sigBlob("\x09".repeat(64)), "HASH");
    assertEq(ok, true);
});

await test("verify_signature reports a bad signature", () => {
    const [ok, err] = hostkey.verifySignature(cryptoStub(false),
        keyBlob(KEY_A), sigBlob("\x09".repeat(64)), "HASH");
    assertEq(ok, false);
    assertEq(err.indexOf("does not verify") >= 0, true, err);
});

await test("a malformed blob is a verification failure, not a crash", () => {
    const [ok, err] = hostkey.verifySignature(cryptoStub(true),
        "garbage", sigBlob("\x09".repeat(64)), "HASH");
    assertEq(ok, false);
    assertEq(typeof err, "string");
});

await test("the signature is checked over the exchange hash", () => {
    let seen;
    const crypto = { ed25519Verify: (data) => { seen = data; return true; } };
    hostkey.verifySignature(crypto, keyBlob(KEY_A), sigBlob("\x09".repeat(64)), "THE-EXCHANGE-HASH");
    assertEq(seen, "THE-EXCHANGE-HASH");
});

// the whole decision -------------------------------------------------------------

await test("verify refuses before it ever looks at the store", async () => {
    const store = newStore({ "spark.local": keyBlob(KEY_A) });
    const d = await hostkey.verify(cryptoStub(false), store,
        "spark.local", keyBlob(KEY_A), sigBlob("\x09".repeat(64)), "H");
    assertEq(d.ok, false);
    assertEq(d.status, undefined);
});

await test("verify reports trusted for a known matching host", async () => {
    const store = newStore({ "spark.local": keyBlob(KEY_A) });
    const d = await hostkey.verify(cryptoStub(true), store,
        "spark.local", keyBlob(KEY_A), sigBlob("\x09".repeat(64)), "H");
    assertEq(d.ok, true);
    assertEq(d.status, hostkey.TRUSTED);
    assertEq(d.fingerprint.slice(0, 7), "SHA256:");
});

await test("verify exposes the fingerprint for an unknown host", async () => {
    const store = newStore();
    const d = await hostkey.verify(cryptoStub(true), store,
        "spark.local", keyBlob(KEY_A), sigBlob("\x09".repeat(64)), "H");
    assertEq(d.status, hostkey.UNKNOWN);
    assertEq(d.fingerprint !== undefined, true);
    assertEq(d.keyBlob, keyBlob(KEY_A));
    assertEq(store.get("spark.local"), undefined);
});

await test("verify never returns trusted for a changed key", async () => {
    const store = newStore({ "spark.local": keyBlob(KEY_A) });
    const d = await hostkey.verify(cryptoStub(true), store,
        "spark.local", keyBlob(KEY_B), sigBlob("\x09".repeat(64)), "H");
    assertEq(d.status, hostkey.CHANGED);
    assertEq(d.status !== hostkey.TRUSTED, true);
});

await test("a changed key reports both fingerprints", async () => {
    const store = newStore({ "spark.local": keyBlob(KEY_A) });
    const d = await hostkey.verify(cryptoStub(true), store,
        "spark.local", keyBlob(KEY_B), sigBlob("\x09".repeat(64)), "H");
    assertEq(d.storedFingerprint !== undefined, true);
    assertEq(d.fingerprint !== d.storedFingerprint, true);
});

await test("a valid signature does not by itself mean trusted", async () => {
    const store = newStore();
    const d = await hostkey.verify(cryptoStub(true), store,
        "new.local", keyBlob(KEY_A), sigBlob("\x09".repeat(64)), "H");
    assertEq(d.ok, true);
    assertEq(d.status, hostkey.UNKNOWN);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
