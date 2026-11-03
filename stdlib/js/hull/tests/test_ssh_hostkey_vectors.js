// test_ssh_hostkey_vectors.js - ECDSA and RSA host keys against OpenSSH/OpenSSL
// (JS twin of test_ssh_hostkey_vectors.lua).
//
// The vectors are not Hull's own output: each key was made by ssh-keygen, the
// stand-in exchange hash H signed by OpenSSL, and `spki` is `openssl pkey
// -pubout` of the key. The Lua suite runs twice (vanilla: 13, then with real
// hull.crypto: 16); this one always has the capability layer, so it runs the
// stand-in cases and the real-crypto cases in one pass (16).

import { hostkey } from "hull:ssh:hostkey";
import { wire } from "hull:ssh:wire";
import { encoding } from "hull:encoding";
import { bytes } from "hull:ssh:bytes";

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

const H = "hull ssh host key test vector: exchange hash stand-in";
const RSA_KEY = "AAAAB3NzaC1yc2EAAAADAQABAAABAQCWnUmK7Fg7+JHycrr5gPCqW6rEBQWf7ZSEyX3heJGIpKaxBSAOyaRVtm61F6p43BmlsHxIDUEmwq/J4Ett7zyvg+FyCAVsbvxftv20BDjNVSQ+zVEg83u/KyA944MQFs98+A14JYeNI9zurhHI7ZggXY99oYriYQtiuvPLnn0mfa7s226iwFqOVXN8Tx5SxKNN8OXsajy0ZWcSWvXcuxJO7hKmA2pB9J07Qm2hWydYa8+idFklDQ05ffM3tPp8UMJOa+lz6TFC7auaeSPWV+v8U+b/2ITPeAHQ0hnW6OpxI2stTaMkWIPHGbuhvAG2OlpQAn71mykQD+EdLlkUzDDl";
const RSA_SPKI = `-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAlp1JiuxYO/iR8nK6+YDw
qluqxAUFn+2UhMl94XiRiKSmsQUgDsmkVbZutReqeNwZpbB8SA1BJsKvyeBLbe88
r4PhcggFbG78X7b9tAQ4zVUkPs1RIPN7vysgPeODEBbPfPgNeCWHjSPc7q4RyO2Y
IF2PfaGK4mELYrrzy559Jn2u7NtuosBajlVzfE8eUsSjTfDl7Go8tGVnElr13LsS
Tu4SpgNqQfSdO0JtoVsnWGvPonRZJQ0NOX3zN7T6fFDCTmvpc+kxQu2rmnkj1lfr
/FPm/9iEz3gB0NIZ1ujqcSNrLU2jJFiDxxm7obwBtjpaUAJ+9ZspEA/hHS5ZFMww
5QIDAQAB
-----END PUBLIC KEY-----
`;
const VECTORS = [
    {
        alg: "ecdsa-sha2-nistp256",
        key: "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBL3c2BPxY64bhKyv11Wqh1J22jEDubjhu4FQmcojpNAF4TMAfedM6+xyyxtrvW9HZOllbuE3G4iuR+C4NSeqPXg=",
        sig: "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAABJAAAAIQCQymxdoon5F6gQuxLD2OHe7MUgOjrpDhiZ0G6nYLKqeAAAACBqpy1qXvgw6GPH2qUZZP27IH58EaZJbOtPTVMOogTxUQ==",
        spki: `-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEvdzYE/FjrhuErK/XVaqHUnbaMQO5
uOG7gVCZyiOk0AXhMwB950zr7HLLG2u9b0dk6WVu4TcbiK5H4Lg1J6o9eA==
-----END PUBLIC KEY-----
`,
    },
    {
        alg: "ecdsa-sha2-nistp384",
        key: "AAAAE2VjZHNhLXNoYTItbmlzdHAzODQAAAAIbmlzdHAzODQAAABhBFThDV4Qyrw48piGnOHBu8w3gapf+zDAGTT77oNAeQg70Gz6Mo1DJaPDkhTih6ZYJ69nIcQfvbeONoKNSD1kCSPd3Ky16RFZingrp/SgC8pJsba9CyrY3VSNKQfwOdgyGg==",
        sig: "AAAAE2VjZHNhLXNoYTItbmlzdHAzODQAAABoAAAAMHzUbfOB/74kZfVz1+0appD0nJAmvqEhegdQGCnBfP2F1mfhuPTXqKWAjd1mRX/eIwAAADBT2+jU8LB2CKSW5n+P/kTwcHibH48nSS8nNmWn5SfdhQlA7vcIjE66GggJitBlxpk=",
        spki: `-----BEGIN PUBLIC KEY-----
MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEVOENXhDKvDjymIac4cG7zDeBql/7MMAZ
NPvug0B5CDvQbPoyjUMlo8OSFOKHplgnr2chxB+9t442go1IPWQJI93crLXpEVmK
eCun9KALykmxtr0LKtjdVI0pB/A52DIa
-----END PUBLIC KEY-----
`,
    },
    {
        alg: "rsa-sha2-256",
        key: RSA_KEY,
        sig: "AAAADHJzYS1zaGEyLTI1NgAAAQA1fNHavkjPVzZ4JFrTIDU7sti6d7g7e7ePug+585iQXCQbfyS3qWIrVVdaHFqmqOIvegupVs0GYp7xAo8q31oHr47WuZg+FLiu0OICtUlLgaa6pXegLAwMYaz65tGt6qxfSJz9Eiuse2RLnDh8OuTbpWxnI2yBPI++X1wN7uce17Gwone++46lLCKFYXftDz7aC49XiL6vsx5YHgZACREMzeEVChRAb6/ikbgX0KSw8bOctQyuSYnfk4sXoqC7uiVTfc1CfSQhff3zoV21fWwzuSjjeY7ZpNLyhIVm5VUECDDsvJBkZtVIxBGjcqo3A6ROOzFMPlRXgaOyCMjBn0P7",
        spki: RSA_SPKI,
    },
    {
        alg: "rsa-sha2-512",
        key: RSA_KEY,
        sig: "AAAADHJzYS1zaGEyLTUxMgAAAQBlvST03KDNBAG1taeds/7+jJdAx+2CK4DhsHxgZfWuQEnBjDybLSs0KK7wzAlAZAO+FBqqycqUflU1/P2DYV8VBDDVOpmJB51Hv4nsl8W1PNXCsfosm3oEXxJd26bHlNK+8k3vZgOaLtqRMFH/tABu22hr9WmQPvrGs9BXdnGypTZY9OLuUjje+h5BmxAnic9evq4JWOC9MvAkcaBHA6Gacop1vgSxu8G7WUl5UAamxHwNix+bgcD2uZm+/HlCtX0Hc93tdffEciWuRAoeBBGQk5lEKdnsYmeGYa5YH0XrxLAGA+IesbxTtMmeenzn/CG/VEZ4Jm8LcJ2yUIUyHqS2",
        spki: RSA_SPKI,
    },
];

const base64 = encoding.base64;
const crypto = bytes.crypto;

function vec(alg) {
    for (const v of VECTORS) if (v.alg === alg) return v;
    throw new Error("no vector " + alg);
}
const blob = (v) => base64.decode(v.key);
const sigb = (v) => base64.decode(v.sig);

// parsing and the PEM bridge -------------------------------------------------

await test("OpenSSH ECDSA and RSA key blobs parse", () => {
    let k = hostkey.parseKey(blob(vec("ecdsa-sha2-nistp256")));
    assertEq(k.type, "ecdsa-sha2-nistp256");
    assertEq(k.q.length, 65);
    k = hostkey.parseKey(blob(vec("ecdsa-sha2-nistp384")));
    assertEq(k.q.length, 97);
    k = hostkey.parseKey(blob(vec("rsa-sha2-256")));
    assertEq(k.type, "ssh-rsa");
    assertEq(k.bits, 2048);
    assertEq(k.e, "\x01\x00\x01");
});

await test("the PEM built from each blob is byte-for-byte OpenSSL's", () => {
    for (const v of VECTORS) {
        assertEq(hostkey.publicKeyPem(hostkey.parseKey(blob(v))), v.spki, v.alg);
    }
});

await test("an ECDSA signature becomes fixed-width r || s", () => {
    let s = hostkey.parseSignature(sigb(vec("ecdsa-sha2-nistp256")));
    assertEq(s.signature.length, 64);
    s = hostkey.parseSignature(sigb(vec("ecdsa-sha2-nistp384")));
    assertEq(s.signature.length, 96);
});

await test("verify hands crypto.verify the right algorithm, key and data", () => {
    const want = { "ecdsa-sha2-nistp256": "ES256", "ecdsa-sha2-nistp384": "ES384",
                   "rsa-sha2-256": "RS256", "rsa-sha2-512": "RS512" };
    for (const v of VECTORS) {
        const seen = {};
        const stub = { verify: (jose, pem, data, sig) => {
            seen.jose = jose; seen.pem = pem; seen.data = data; seen.len = sig.length;
            return true;
        } };
        assertEq(hostkey.verifySignature(stub, blob(v), sigb(v), H, v.alg)[0], true, v.alg);
        assertEq(seen.jose, want[v.alg], v.alg);
        assertEq(seen.pem, v.spki, v.alg);
        assertEq(seen.data, H, v.alg);
        if (v.alg.indexOf("rsa") >= 0) assertEq(seen.len, 256, v.alg);
    }
});

// the negotiated algorithm decides ---------------------------------------------

const always = () => ({ verify: () => true });

await test("a signature under another algorithm than the one negotiated is refused", () => {
    const v = vec("rsa-sha2-256");
    const [ok, why] = hostkey.verifySignature(always(), blob(v), sigb(v), H, "rsa-sha2-512");
    assertEq(ok, false);
    assertEq(why.indexOf("negotiated") >= 0, true, why);
});

await test("a key cannot sign under an algorithm for another key type", () => {
    const k = blob(vec("ecdsa-sha2-nistp256"));
    const s = sigb(vec("rsa-sha2-256"));
    const [ok, why] = hostkey.verifySignature(always(), k, s, H, "rsa-sha2-256");
    assertEq(ok, false);
    assertEq(why.indexOf("cannot sign") >= 0, true, why);
});

await test("SHA-1 ssh-rsa signatures are not accepted", () => {
    const v = vec("rsa-sha2-256");
    const inner = wire.reader(sigb(v)); inner.string();
    const sha1 = wire.writer().string("ssh-rsa").string(inner.string()).build();
    const [ok] = hostkey.verifySignature(always(), blob(v), sha1, H);
    assertEq(ok, false);
});

// keys that are refused -------------------------------------------------------

const rsaBlob = (e, n) => wire.writer().string("ssh-rsa").mpint(e).mpint(n).build();

await test("an RSA key below 2048 bits is refused", () => {
    const n1024 = "\xc0" + "\x01".repeat(127);
    let ok = true, err;
    try { hostkey.parseKey(rsaBlob("\x01\x00\x01", n1024)); } catch (e) { ok = false; err = e; }
    assertEq(ok, false);
    assertEq(String(err && err.message).indexOf("minimum") >= 0, true, String(err));
});

await test("an RSA key with an even or unit exponent is refused", () => {
    const n = "\xc0" + "\x01".repeat(255);
    assertRaises(() => hostkey.parseKey(rsaBlob("\x01\x00\x00", n)), "even e");
    assertRaises(() => hostkey.parseKey(rsaBlob("\x01", n)), "e = 1");
});

await test("a compressed or wrong-curve ECDSA point is refused", () => {
    const q = "\x02" + "\x01".repeat(32);
    assertRaises(() => hostkey.parseKey(wire.writer().string("ecdsa-sha2-nistp256")
        .string("nistp256").string(q).build()), "compressed");
    assertRaises(() => hostkey.parseKey(wire.writer().string("ecdsa-sha2-nistp256")
        .string("nistp384").string("\x04" + "\x01".repeat(64)).build()), "curve mismatch");
});

await test("a key blob with trailing bytes is refused", () => {
    assertRaises(() => hostkey.parseKey(blob(vec("rsa-sha2-256")) + "\x00"));
});

await test("an RSA signature longer than the modulus is refused", () => {
    const v = vec("rsa-sha2-256");
    const long = wire.writer().string("rsa-sha2-256").string("\x01".repeat(257)).build();
    const [ok, why] = hostkey.verifySignature(always(), blob(v), long, H, "rsa-sha2-256");
    assertEq(ok, false);
    assertEq(why.indexOf("longer than the modulus") >= 0, true, why);
});

await test("a short RSA signature is padded to the modulus length", () => {
    const v = vec("rsa-sha2-256");
    let seen;
    const stub = { verify: (_a, _b, _c, sig) => { seen = sig; return true; } };
    const short = wire.writer().string("rsa-sha2-256").string("\x07".repeat(250)).build();
    hostkey.verifySignature(stub, blob(v), short, H, "rsa-sha2-256");
    assertEq(seen.length, 256);
    assertEq(seen.slice(0, 6), "\x00".repeat(6));
});

// against the real hull:crypto (bytes.crypto) ----------------------------------

await test("the OpenSSL signatures verify through hull.crypto", () => {
    for (const v of VECTORS) {
        const [ok, why] = hostkey.verifySignature(crypto, blob(v), sigb(v), H, v.alg);
        assertEq(ok, true, v.alg + " " + String(why));
    }
});

await test("a different exchange hash does not verify", () => {
    for (const v of VECTORS) {
        const [ok] = hostkey.verifySignature(crypto, blob(v), sigb(v), H + "!", v.alg);
        assertEq(ok, false, v.alg);
    }
});

await test("a signature with one byte changed does not verify", () => {
    for (const v of VECTORS) {
        const s = sigb(v);
        const i = s.length - 4;     // Lua's #s - 3, as a 0-based index
        const bad = s.slice(0, i) + String.fromCharCode(s.charCodeAt(i) ^ 1) + s.slice(i + 1);
        const [ok] = hostkey.verifySignature(crypto, blob(v), bad, H, v.alg);
        assertEq(ok, false, v.alg);
    }
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
