// test_ssh_kexinit.js - Tests for hull:ssh:kexinit (twin of test_ssh_kexinit.lua)
//
// RFC 4253 section 7.1. The negotiation cases matter most: this is where a
// server gets to influence which algorithms Hull will then use.

import { kexinit } from "hull:ssh:kexinit";
import { wire } from "hull:ssh:wire";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try { await fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + (e && e.stack || e)); }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}

function assertRaises(fn, msg) {
    let ok = true;
    try { fn(); } catch (e) { ok = false; }
    if (ok) throw new Error((msg || "should have raised") + " but did not");
}

const has = (s, sub) => typeof s === "string" && s.indexOf(sub) >= 0;

const COOKIE = "\x07".repeat(16);

// a server KEXINIT that agrees with everything Hull offers
function agreeable() {
    return {
        cookie: COOKIE,
        kex: ["curve25519-sha256"],
        hostKey: ["ssh-ed25519"],
        cipherC2s: ["aes256-gcm@openssh.com"],
        cipherS2c: ["aes256-gcm@openssh.com"],
        macC2s: ["hmac-sha2-256"],
        macS2c: ["hmac-sha2-256"],
        compressionC2s: ["none"],
        compressionS2c: ["none"],
        languagesC2s: [], languagesS2c: [],
        firstKexPacketFollows: false, reserved: 0,
    };
}

// build and parse --------------------------------------------------------

await test("build produces a parseable KEXINIT", () => {
    const p = kexinit.build(null, COOKIE);
    assertEq(p.charCodeAt(0), kexinit.SSH_MSG_KEXINIT);
    const got = kexinit.parse(p);
    assertEq(got.cookie, COOKIE);
    assertEq(got.kex[0], "curve25519-sha256");
    assertEq(got.hostKey[0], "ssh-ed25519");
    assertEq(got.cipherC2s[0], "aes256-gcm@openssh.com");
    assertEq(got.compressionC2s[0], "none");
    assertEq(got.firstKexPacketFollows, false);
    assertEq(got.reserved, 0);
});

await test("the strict-KEX marker is sent even when a caller's kex list drops it", () => {
    for (const opts of [null, { kex: ["curve25519-sha256"] }]) {
        const o = opts ? Object.assign({}, kexinit.DEFAULT_OFFER, opts) : null;
        const got = kexinit.parse(kexinit.build(o, COOKIE));
        let seen = 0;
        for (const name of got.kex) if (name === kexinit.STRICT_C) seen++;
        assertEq(seen, 1);
        assertEq(got.kex[0], "curve25519-sha256");
    }
});

await test("build demands a 16-byte cookie", () => {
    assertRaises(() => kexinit.build(null, "short"), "short cookie");
    assertRaises(() => kexinit.build(null, null), "no cookie");
    assertRaises(() => kexinit.build(null, "x".repeat(17)), "long cookie");
});

await test("Hull never sets first_kex_packet_follows", () => {
    assertEq(kexinit.parse(kexinit.build(null, COOKIE)).firstKexPacketFollows, false);
});

await test("parse rejects a different message type", () => {
    const p = String.fromCharCode(21) + "\x00".repeat(40);
    assertRaises(() => kexinit.parse(p), "wrong msg type");
});

await test("parse rejects a truncated message", () => {
    const p = kexinit.build(null, COOKIE);
    assertRaises(() => kexinit.parse(p.slice(0, 20)), "truncated");
});

await test("parse tolerates trailing bytes", () => {
    const p = kexinit.build(null, COOKIE) + "future extension";
    assertEq(kexinit.parse(p).kex[0], "curve25519-sha256");
});

await test("compression is offered as none only", () => {
    const got = kexinit.parse(kexinit.build(null, COOKIE));
    assertEq(got.compressionC2s.length, 1);
    assertEq(got.compressionC2s[0], "none");
});

// choose ------------------------------------------------------------------

await test("choose honours the client order, not the server order", () => {
    assertEq(kexinit.choose(["a", "b"], ["b", "a"]), "a");
});

await test("choose returns nil when there is no overlap", () => {
    assertEq(kexinit.choose(["a"], ["b"]), null);
    assertEq(kexinit.choose([], ["a"]), null);
    assertEq(kexinit.choose(["a"], []), null);
});

// negotiate ----------------------------------------------------------------

await test("negotiate agrees with a cooperative server", () => {
    const [n, err] = kexinit.negotiate(null, agreeable());
    assertEq(err, undefined);
    assertEq(n.kex, "curve25519-sha256");
    assertEq(n.hostKey, "ssh-ed25519");
    assertEq(n.cipherC2s, "aes256-gcm@openssh.com");
    assertEq(n.cipherS2c, "aes256-gcm@openssh.com");
    assertEq(n.compressionC2s, "none");
});

await test("negotiate accepts the libssh name for the same exchange", () => {
    const s = agreeable();
    s.kex = ["curve25519-sha256@libssh.org"];
    assertEq(kexinit.negotiate(null, s)[0].kex, "curve25519-sha256@libssh.org");
});

await test("negotiate refuses a server with no common cipher", () => {
    const s = agreeable();
    s.cipherS2c = ["3des-cbc", "aes128-cbc"];
    const [n, err] = kexinit.negotiate(null, s);
    assertEq(n, null);
    assertEq(has(err, "server-to-client cipher"), true, err);
    assertEq(has(err, "3des-cbc"), true, err);
});

await test("negotiate refuses a server offering only weak kex", () => {
    const s = agreeable();
    s.kex = ["diffie-hellman-group1-sha1"];
    const [n, err] = kexinit.negotiate(null, s);
    assertEq(n, null);
    assertEq(has(err, "key exchange"), true, err);
});

await test("negotiate settles on RSA with SHA-2 when that is all the server has", () => {
    const s = agreeable();
    s.hostKey = ["ssh-rsa", "rsa-sha2-256", "rsa-sha2-512"];
    assertEq(kexinit.negotiate(null, s)[0].hostKey, "rsa-sha2-512");
});

await test("negotiate refuses a server offering only SHA-1 RSA signatures", () => {
    const s = agreeable();
    s.hostKey = ["ssh-rsa", "ssh-dss"];
    const [n, err] = kexinit.negotiate(null, s);
    assertEq(n, null);
    assertEq(has(err, "host key"), true, err);
});

await test("negotiate prefers Ed25519 over ECDSA and RSA", () => {
    const s = agreeable();
    s.hostKey = ["rsa-sha2-512", "ecdsa-sha2-nistp256", "ssh-ed25519"];
    assertEq(kexinit.negotiate(null, s)[0].hostKey, "ssh-ed25519");
});

await test("negotiate refuses compression", () => {
    const s = agreeable();
    s.compressionC2s = ["zlib@openssh.com"];
    s.compressionS2c = ["zlib@openssh.com"];
    const [n, err] = kexinit.negotiate(null, s);
    assertEq(n, null);
    assertEq(has(err, "compression"), true, err);
});

await test("a missing MAC is fine under an AEAD cipher", () => {
    const s = agreeable();
    s.macC2s = [];
    s.macS2c = [];
    const [n, err] = kexinit.negotiate(null, s);
    assertEq(err, undefined);
    assertEq(n.macC2s, null);
    assertEq(n.cipherC2s, "aes256-gcm@openssh.com");
});

await test("an empty server offer is refused, not defaulted", () => {
    const s = agreeable();
    s.kex = [];
    assertEq(kexinit.negotiate(null, s)[0], null);
});

// guessed packets ------------------------------------------------------------

await test("a guess that matches is not discarded", () => {
    const s = agreeable();
    s.firstKexPacketFollows = true;
    const [n] = kexinit.negotiate(null, s);
    assertEq(kexinit.guessWasWrong(s, n), false);
});

await test("a guess on a different kex is discarded", () => {
    const s = agreeable();
    s.firstKexPacketFollows = true;
    s.kex = ["diffie-hellman-group14-sha256", "curve25519-sha256"];
    const [n] = kexinit.negotiate(null, s);
    assertEq(n.kex, "curve25519-sha256");
    assertEq(kexinit.guessWasWrong(s, n), true);
});

await test("no guess means nothing to discard", () => {
    const s = agreeable();
    const [n] = kexinit.negotiate(null, s);
    assertEq(kexinit.guessWasWrong(s, n), false);
});

// chacha20-poly1305 and strict KEX ----------------------------------------

const CHACHA = "chacha20-poly1305@openssh.com";

function chachaOnly(strict) {
    const s = agreeable();
    s.cipherC2s = [CHACHA];
    s.cipherS2c = [CHACHA];
    if (strict) s.kex = ["curve25519-sha256", kexinit.STRICT_S];
    return s;
}

await test("chacha20-poly1305 is chosen when the server is strict and offers only it", () => {
    const [n, err] = kexinit.negotiate(null, chachaOnly(true));
    assertEq(err, undefined);
    assertEq(n.cipherC2s, CHACHA);
    assertEq(n.cipherS2c, CHACHA);
});

await test("chacha20-poly1305 is never chosen without strict KEX (Terrapin)", () => {
    const [n, err] = kexinit.negotiate(null, chachaOnly(false));
    assertEq(n, null);
    assertEq(typeof err, "string");
});

await test("AES-GCM still wins when the server offers both", () => {
    const s = chachaOnly(true);
    s.cipherC2s = [CHACHA, "aes256-gcm@openssh.com"];
    s.cipherS2c = [CHACHA, "aes256-gcm@openssh.com"];
    assertEq(kexinit.negotiate(null, s)[0].cipherC2s, "aes256-gcm@openssh.com");
});

await test("an offer of chacha alone against a non-strict server says why", () => {
    const [, err] = kexinit.negotiate({ kex: ["curve25519-sha256"],
                                        hostKey: ["ssh-ed25519"],
                                        cipher: [CHACHA],
                                        mac: ["hmac-sha2-256"],
                                        compression: ["none"] },
                                      chachaOnly(false));
    assertEq(has(err, "strict KEX"), true, err);
});

await test("on a rekey the connection's strictness decides, not the new KEXINIT", () => {
    const s = chachaOnly(false);           // no marker, as in a rekey
    assertEq(kexinit.negotiate(null, s, true)[0].cipherC2s, CHACHA);
    assertEq(kexinit.negotiate(null, chachaOnly(true), false)[0], null);
});

await test("the default offer lists only algorithms Hull can do", async () => {
    const o = kexinit.DEFAULT_OFFER;
    assertEq(o.cipher.join(","), "aes256-gcm@openssh.com,chacha20-poly1305@openssh.com");
    assertEq(o.hostKey.join(","),
        "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,rsa-sha2-512,rsa-sha2-256");
    // Every one of them is an algorithm hull:ssh:hostkey verifies (loaded
    // lazily, as the Lua test requires it inside the case).
    const { hostkey } = await import("hull:ssh:hostkey");
    for (const name of o.hostKey) {
        assertEq(hostkey.ALGORITHMS[name] !== undefined && hostkey.ALGORITHMS[name] !== null,
            true, name);
    }
    assertEq(o.compression.length, 1);
    assertEq(o.compression[0], "none");
    for (const name of o.kex) {
        if (name !== kexinit.STRICT_C) assertEq(has(name, "curve25519"), true, name);
    }
});

// strict KEX (Terrapin, CVE-2023-48795) ---------------------------------------

await test("the offer advertises strict KEX, and last", () => {
    const o = kexinit.DEFAULT_OFFER;
    assertEq(o.kex[o.kex.length - 1], kexinit.STRICT_C,
        "the marker must never be preferred over a real exchange:");
});

await test("a server advertising strict KEX is detected", () => {
    assertEq(kexinit.serverIsStrict({ kex: ["curve25519-sha256", kexinit.STRICT_S] }), true);
    assertEq(kexinit.serverIsStrict({ kex: ["curve25519-sha256"] }), false);
    assertEq(kexinit.serverIsStrict({}), false);
});

await test("a server echoing our own marker cannot win the negotiation with it", () => {
    const [neg, err] = kexinit.negotiate(null, {
        kex: [kexinit.STRICT_C, kexinit.STRICT_S],
        hostKey: ["ssh-ed25519"],
        cipherC2s: ["aes256-gcm@openssh.com"],
        cipherS2c: ["aes256-gcm@openssh.com"],
        macC2s: [], macS2c: [],
        compressionC2s: ["none"], compressionS2c: ["none"],
    });
    assertEq(neg, null, "must not negotiate a marker as the exchange");
    assertEq(has(err, "no common key exchange"), true, String(err));
});

await test("strict KEX does not disturb an ordinary negotiation", () => {
    const [neg] = kexinit.negotiate(null, {
        kex: ["curve25519-sha256", kexinit.STRICT_S],
        hostKey: ["ssh-ed25519"],
        cipherC2s: ["aes256-gcm@openssh.com"],
        cipherS2c: ["aes256-gcm@openssh.com"],
        macC2s: [], macS2c: [],
        compressionC2s: ["none"], compressionS2c: ["none"],
    });
    assertEq(neg !== null && neg.kex, "curve25519-sha256");
});

await test("the built message round trips through the wire codec", () => {
    const p = kexinit.build(null, COOKIE);
    const r = wire.reader(p);
    assertEq(r.byte(), 20);
    assertEq(r.raw(16), COOKIE);
    assertEq(r.namelist()[0], "curve25519-sha256");
});

// validate_offer ----------------------------------------------------------------

function copyOffer() {
    const o = {};
    for (const k of Object.keys(kexinit.DEFAULT_OFFER)) o[k] = kexinit.DEFAULT_OFFER[k].slice();
    return o;
}

await test("the default offer, and no offer at all, are valid", () => {
    assertEq(kexinit.validateOffer(null)[0], true);
    assertEq(kexinit.validateOffer(kexinit.DEFAULT_OFFER)[0], true);
});

await test("an offer may narrow and reorder", () => {
    const o = copyOffer();
    o.kex = ["curve25519-sha256@libssh.org", "curve25519-sha256"];
    assertEq(kexinit.validateOffer(o)[0], true);
});

await test("an offer naming an unimplemented algorithm is refused", () => {
    const cases = { kex: "diffie-hellman-group14-sha256", hostKey: "ssh-rsa",
                    cipher: "aes128-ctr", compression: "zlib" };
    for (const list of Object.keys(cases)) {
        const name = cases[list];
        const o = copyOffer();
        o[list] = [name];
        const [ok, why] = kexinit.validateOffer(o);
        assertEq(ok, null, list);
        assertEq(has(why, name), true, why);
    }
});

await test("an offer with no real algorithm in a list is refused", () => {
    let o = copyOffer();
    o.kex = [kexinit.STRICT_C];          // a marker is not an exchange
    assertEq(kexinit.validateOffer(o)[0], null);
    o = copyOffer();
    o.cipher = [];
    assertEq(kexinit.validateOffer(o)[0], null);
});

await test("ssh.connect refuses a bad offer before dialling anything", async () => {
    const o = copyOffer();
    o.cipher = ["arcfour"];
    let dialled = false;
    const { ssh } = await import("hull:ssh");
    let ok = true, err = null;
    // The crypto under test goes in through hull:_test_hooks ("hull:ssh"):
    // a `crypto` option on a public hull:ssh entry point is refused (audit 44).
    const hooks = __testHooks("hull:ssh");
    hooks.setCrypto({ random: (n) => "\x00".repeat(n) });
    try {
        await ssh.connect({
            host: "h", user: "u", key: {}, offer: o,
            openStream: () => { dialled = true; },
        });
    } catch (e) { ok = false; err = e; } finally { hooks.setCrypto(null); }
    assertEq(ok, false);
    assertEq(has(String(err && err.message), "arcfour"), true, String(err));
    assertEq(dialled, false);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
