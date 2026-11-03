// test_ssh_privatekey.js - Tests for hull:ssh:privatekey (JS twin of
// test_ssh_privatekey.lua).
//
// The key file is BUILT here rather than embedded, so the repository carries
// no blob that looks like a private key, and each malformation is exact.

import { encoding } from "hull:encoding";
import { privatekey } from "hull:ssh:privatekey";
import { wire } from "hull:ssh:wire";

const base64 = encoding.base64;

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try { await fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + ((e && e.message) || e) + "\n" + ((e && e.stack) || "")); }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}

function assertRaises(fn, msg) {
    let ok = true;
    try { fn(); } catch (e) { ok = false; }
    if (ok) throw new Error((msg || "should have raised") + " but did not");
}

// Lua's pcall: [true, value] | [false, err]
function pcall(fn, ...args) {
    try { return [true, fn(...args)]; } catch (e) { return [false, e]; }
}
const errText = (e) => (e && typeof e === "object" && typeof e.message === "string")
    ? e.message : String(e);

// armour ----------------------------------------------------------------------

await test("armour decode skips line breaks but refuses junk", () => {
    const wrap = privatekey.BEGIN + "\nZm9v\nYmFy\r\n" + privatekey.END + "\n";
    assertEq(privatekey.unarmour(wrap), "foobar");
    const bad = privatekey.BEGIN + "\nZm9v!YmFy\n" + privatekey.END + "\n";
    assertRaises(() => privatekey.unarmour(bad), "punctuation");
});

// building a key file ---------------------------------------------------------

const PUB = "\x0b".repeat(32);
const SECRET = "\x16".repeat(32) + PUB;      // seed || public
const COMMENT = "test@hull";

const pubBlob = (pub) => wire.writer().string("ssh-ed25519").string(pub || PUB).build();

function privateSection(opts) {
    opts = opts || {};
    const check = opts.check || 0x01020304;
    const w = wire.writer();
    w.uint32(check);
    w.uint32(opts.check2 || check);
    w.string(opts.algo || "ssh-ed25519");
    w.string(opts.pub || PUB);
    w.string(opts.secret || SECRET);
    w.string(COMMENT);
    const body = w.build();
    let pad = "";
    let i = 1;
    while ((body.length + pad.length) % 8 !== 0) { pad += String.fromCharCode(i); i++; }
    return body + pad;
}

function keyFile(opts) {
    opts = opts || {};
    const w = wire.writer();
    w.raw(privatekey.MAGIC);
    w.string(opts.cipher || "none");
    w.string(opts.kdf || "none");
    w.string(opts.kdfopts || "");
    w.uint32(opts.nkeys || 1);
    w.string(opts.publicBlob || pubBlob());
    w.string(opts.privateBlob || privateSection(opts));
    const armoured = base64.encode(w.build());
    return privatekey.BEGIN + "\n" + armoured + "\n" + privatekey.END + "\n";
}

// loading -----------------------------------------------------------------------

await test("loads a well-formed key", () => {
    const k = privatekey.load(keyFile());
    assertEq(k.algorithm, "ssh-ed25519");
    assertEq(k.public, PUB);
    assertEq(k.secret, SECRET);
    assertEq(k.comment, COMMENT);
    assertEq(k.blob, pubBlob());
});

await test("the secret half is passed through at 64 bytes", () => {
    const k = privatekey.load(keyFile());
    assertEq(k.secret.length, 64);
    assertEq(k.secret.slice(32), k.public);
});

await test("refuses a PEM or PKCS8 key by name", () => {
    const [ok, err] = pcall(privatekey.load,
        "-----BEGIN PRIVATE KEY-----\nMC4CAQ==\n-----END PRIVATE KEY-----\n");
    assertEq(ok, false);
    assertEq(errText(err).indexOf("PEM") >= 0, true, errText(err));
});

await test("refuses a truncated file", () => {
    assertRaises(() => privatekey.load(privatekey.BEGIN + "\nZm9v\n"), "no END line");
});

await test("refuses bad magic", () => {
    const body = base64.encode("not-a-key-v1\x00" + "\x00".repeat(40));
    assertRaises(() => privatekey.load(privatekey.BEGIN + "\n" + body + "\n" + privatekey.END), "magic");
});

// encrypted keys -------------------------------------------------------------------

await test("an encrypted key with no passphrase says how to give one", () => {
    const [ok, err] = pcall(privatekey.load, keyFile({ cipher: "aes256-ctr", kdf: "bcrypt" }));
    assertEq(ok, false);
    const t = errText(err);
    // PARITY: the JS option is named passphraseEnv (Lua: passphrase_env).
    assertEq(t.indexOf("passphraseEnv") >= 0, true, t);
    assertEq(t.indexOf("passphrase") >= 0, true, t);
});

await test("refuses a cipher it does not read, by name", () => {
    const [ok, err] = pcall(privatekey.load,
        keyFile({ cipher: "aes128-cbc", kdf: "bcrypt" }), { passphrase: "x" });
    assertEq(ok, false);
    const t = errText(err);
    assertEq(t.indexOf("aes128-cbc") >= 0, true, t);
    assertEq(t.indexOf("ssh-keygen -p") >= 0, true, t);
});

await test("malformed kdfoptions read as a malformed KEY", () => {
    const [ok, err] = pcall(privatekey.load,
        keyFile({ cipher: "aes256-ctr", kdf: "bcrypt" }), { passphrase: "x" });
    assertEq(ok, false);
    const t = errText(err);
    assertEq(t.indexOf("kdfoptions") >= 0, true, t);
});

await test("parse_container exposes an encrypted key without refusing", () => {
    const raw = privatekey.unarmour(keyFile({ cipher: "aes256-ctr", kdf: "bcrypt" }));
    const c = privatekey.parseContainer(raw);
    assertEq(c.encrypted, true);
    assertEq(c.cipher, "aes256-ctr");
});

// malformed keys ----------------------------------------------------------------------

await test("refuses mismatched check bytes", () => {
    const [ok, err] = pcall(privatekey.load,
        keyFile({ privateBlob: privateSection({ check: 1, check2: 2 }) }));
    assertEq(ok, false);
    assertEq(errText(err).indexOf("check bytes") >= 0, true, errText(err));
});

await test("refuses an unsupported key type", () => {
    const [ok, err] = pcall(privatekey.load,
        keyFile({ privateBlob: privateSection({ algo: "ecdsa-sha2-nistp521" }) }));
    assertEq(ok, false);
    assertEq(errText(err).indexOf("ecdsa-sha2-nistp521") >= 0, true, errText(err));
});

await test("the refusals a caller acts on carry a code, and read as before", () => {
    const cases = [
        ["unsupported_key_type",
         ["-----BEGIN PRIVATE KEY-----\nMC4CAQ==\n-----END PRIVATE KEY-----\n"]],
        ["unsupported_key_type",
         [keyFile({ privateBlob: privateSection({ algo: "ecdsa-sha2-nistp521" }) })]],
        ["unsupported_key_type",
         [keyFile({ cipher: "aes128-cbc", kdf: "bcrypt" }), { passphrase: "x" }]],
        ["passphrase_required",
         [keyFile({ cipher: "aes256-ctr", kdf: "bcrypt" })]],
        ["passphrase_required",
         [keyFile({ cipher: "aes256-ctr", kdf: "bcrypt" }), { passphrase: "" }]],
    ];
    for (const c of cases) {
        const [ok, err] = pcall(privatekey.load, ...c[1]);
        assertEq(ok, false);
        assertEq(typeof err, "object", c[0]);
        assertEq(err.code, c[0]);
        assertEq(errText(err), err.detail);
        assertEq(errText(err).indexOf("ssh.privatekey: "), 0);
    }
});

await test("a peer-chosen algorithm name is sanitised before it is quoted", () => {
    const [ok, err] = pcall(privatekey.load,
        keyFile({ privateBlob: privateSection({ algo: "ssh-rsa\x1b[31m" }) }));
    assertEq(ok, false);
    assertEq(errText(err).indexOf("\x1b"), -1, errText(err));
});

await test("refuses when the two public copies disagree", () => {
    const other = "\x63".repeat(32);
    const [ok, err] = pcall(privatekey.load, keyFile({
        privateBlob: privateSection({ secret: "\x16".repeat(32) + other }),
    }));
    assertEq(ok, false);
    assertEq(errText(err).indexOf("disagree") >= 0, true, errText(err));
});

await test("refuses when the container blob disagrees with the private key", () => {
    const [ok, err] = pcall(privatekey.load, keyFile({ publicBlob: pubBlob("\x4d".repeat(32)) }));
    assertEq(ok, false);
    assertEq(errText(err).indexOf("does not match") >= 0, true, errText(err));
});

await test("refuses a multi-key file", () => {
    assertRaises(() => privatekey.load(keyFile({ nkeys: 2 })), "two keys");
});

await test("refuses bad padding", () => {
    const body = privateSection();
    const broken = body.slice(0, body.length - 1) + "\x63";
    assertRaises(() => privatekey.load(keyFile({ privateBlob: broken })), "padding");
});

await test("refuses a wrong-length public half", () => {
    assertRaises(() => privatekey.load(keyFile({
        privateBlob: privateSection({ pub: "\x0b".repeat(31) }),
    })), "31-byte public");
});

// fingerprint ----------------------------------------------------------------------------

await test("fingerprint has the OpenSSH shape", () => {
    const k = privatekey.load(keyFile());
    const fp = privatekey.fingerprint(() => "\x00".repeat(32), k);
    assertEq(fp, "SHA256:" + "A".repeat(43));
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
