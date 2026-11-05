// test_ssh_ecdsa_key.js - ECDSA user keys, against keys ssh-keygen wrote (JS
// twin of test_ssh_ecdsa_key.lua).
//
// EC256 / EC384 / EC521 are `ssh-keygen -t ecdsa -b 256|384|521`; EC256_ENC
// is EC256's sibling protected with `ssh-keygen -p`. *_SEC1 is the same key
// after `ssh-keygen -m PEM` (the PEM Hull builds must be byte-for-byte that);
// *_SPKI is `ssh-keygen -e -m PKCS8`. ECDSA signatures are randomised, so the
// crypto half verifies them rather than comparing bytes.
//
// The Lua suite runs the structural half vanilla (5) and everything with the
// real hull.crypto (8). This file always has the capability layer: the
// stand-in cases and the real-crypto cases run in one pass (8).

import { privatekey } from "hull:ssh:privatekey";
import { userauth } from "hull:ssh:userauth";
import { wire } from "hull:ssh:wire";
import { encoding } from "hull:encoding";
import { bytes } from "hull:ssh:bytes";

const base64 = encoding.base64;
const crypto = bytes.crypto;

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try { await fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + ((e && e.message) || e) + "\n" + ((e && e.stack) || "")); }
}

// A case that needs the mbedTLS backend (AES-CTR / GCM, ChaCha20, Poly1305,
// RSA, ECDSA). The JS twin of test_lua.c's #ifdef HL_LINK_TLS real-crypto
// legs: the C harness sets __hull_build from the build's own macros, so a case
// is skipped only in a build without TLS (hull:crypto throws "no ... backend"
// there, as Lua's does), never in one that has it.
const TLS = !globalThis.__hull_build || globalThis.__hull_build.tls !== false;
function realTest(name, fn) {
    if (TLS) return test(name, fn);
    console.log("SKIP: " + name + " (mbedTLS is not linked in this build)");
    return undefined;
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + String(b) + ", got " + String(a));
}

function pcall(fn, ...args) {
    try { return [true, fn(...args)]; } catch (e) { return [false, e]; }
}
const errText = (e) => (e && typeof e === "object" && typeof e.message === "string")
    ? e.message : String(e);

const EC256 = `-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAaAAAABNlY2RzYS
1zaGEyLW5pc3RwMjU2AAAACG5pc3RwMjU2AAAAQQTXIJbvJD1ZRwlCd7dVtumHVA2EbNn+
8P8qucTjO9wnkXR2g2442W4IDqZkk5pNyYOKydgz586rRMhUYF3abIADAAAAqBpKohkaSq
IZAAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBNcglu8kPVlHCUJ3
t1W26YdUDYRs2f7w/yq5xOM73CeRdHaDbjjZbggOpmSTmk3Jg4rJ2DPnzqtEyFRgXdpsgA
MAAAAgVMhUSTzSgrpOsVsqWOgxu60xiq5wGl2xhrvXgHyUjmoAAAAKaHVsbC1lYzI1NgEC
AwQFBg==
-----END OPENSSH PRIVATE KEY-----
`;
const EC256_SEC1 = `-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIFTIVEk80oK6TrFbKljoMbutMYqucBpdsYa714B8lI5qoAoGCCqGSM49
AwEHoUQDQgAE1yCW7yQ9WUcJQne3Vbbph1QNhGzZ/vD/KrnE4zvcJ5F0doNuONlu
CA6mZJOaTcmDisnYM+fOq0TIVGBd2myAAw==
-----END EC PRIVATE KEY-----
`;
const EC256_SPKI = `-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE1yCW7yQ9WUcJQne3Vbbph1QNhGzZ
/vD/KrnE4zvcJ5F0doNuONluCA6mZJOaTcmDisnYM+fOq0TIVGBd2myAAw==
-----END PUBLIC KEY-----
`;
const EC256_PUB = base64.decode("AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBNcglu8kPVlHCUJ3t1W26YdUDYRs2f7w/yq5xOM73CeRdHaDbjjZbggOpmSTmk3Jg4rJ2DPnzqtEyFRgXdpsgAM=");

const EC384 = `-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAiAAAABNlY2RzYS
1zaGEyLW5pc3RwMzg0AAAACG5pc3RwMzg0AAAAYQRnezlrCoYBMLVRLKY0F5H41pXrNsgI
7o77MW5ZPKfGWhgkpRCEbPtto1wAj9cdAzF5OnzrfeNovLJRX+e50O+YMLUlGEvsDGuJjI
cCqwpkSZOc0Rf+c5GjsNhY4e20GxYAAADYK1ivrCtYr6wAAAATZWNkc2Etc2hhMi1uaXN0
cDM4NAAAAAhuaXN0cDM4NAAAAGEEZ3s5awqGATC1USymNBeR+NaV6zbICO6O+zFuWTynxl
oYJKUQhGz7baNcAI/XHQMxeTp8633jaLyyUV/nudDvmDC1JRhL7AxriYyHAqsKZEmTnNEX
/nORo7DYWOHttBsWAAAAMQDtV0CrCTYsp2wiWEu6lzB3uSYXgVBvRnVVsM33fVd1grXyLS
ntaZT3p7pCD3rPkPMAAAAKaHVsbC1lYzM4NAECAwQF
-----END OPENSSH PRIVATE KEY-----
`;
const EC384_SEC1 = `-----BEGIN EC PRIVATE KEY-----
MIGkAgEBBDDtV0CrCTYsp2wiWEu6lzB3uSYXgVBvRnVVsM33fVd1grXyLSntaZT3
p7pCD3rPkPOgBwYFK4EEACKhZANiAARnezlrCoYBMLVRLKY0F5H41pXrNsgI7o77
MW5ZPKfGWhgkpRCEbPtto1wAj9cdAzF5OnzrfeNovLJRX+e50O+YMLUlGEvsDGuJ
jIcCqwpkSZOc0Rf+c5GjsNhY4e20GxY=
-----END EC PRIVATE KEY-----
`;
const EC384_SPKI = `-----BEGIN PUBLIC KEY-----
MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEZ3s5awqGATC1USymNBeR+NaV6zbICO6O
+zFuWTynxloYJKUQhGz7baNcAI/XHQMxeTp8633jaLyyUV/nudDvmDC1JRhL7Axr
iYyHAqsKZEmTnNEX/nORo7DYWOHttBsW
-----END PUBLIC KEY-----
`;
const EC384_PUB = base64.decode("AAAAE2VjZHNhLXNoYTItbmlzdHAzODQAAAAIbmlzdHAzODQAAABhBGd7OWsKhgEwtVEspjQXkfjWles2yAjujvsxblk8p8ZaGCSlEIRs+22jXACP1x0DMXk6fOt942i8slFf57nQ75gwtSUYS+wMa4mMhwKrCmRJk5zRF/5zkaOw2Fjh7bQbFg==");

const EC521 = `-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAArAAAABNlY2RzYS
1zaGEyLW5pc3RwNTIxAAAACG5pc3RwNTIxAAAAhQQBdJ+EL6DZ/b0MLtBZ6zXWa9ukFLaJ
lPHN2WjKz2fYyyHqA0mqAl+yT9VW5JfSJ9OO8LUaJTiQuh0tXPsY7kfmyT4A7PCPHjJ8/1
1Qfe+OOOQ21AmJKmS85SvYNO7hTdiN1BNAQIejfPV5brhiNef79f1nxC6acEZ+hwQ8Rngb
VmQwUmUAAAEIhUMfS4VDH0sAAAATZWNkc2Etc2hhMi1uaXN0cDUyMQAAAAhuaXN0cDUyMQ
AAAIUEAXSfhC+g2f29DC7QWes11mvbpBS2iZTxzdloys9n2Msh6gNJqgJfsk/VVuSX0ifT
jvC1GiU4kLodLVz7GO5H5sk+AOzwjx4yfP9dUH3vjjjkNtQJiSpkvOUr2DTu4U3YjdQTQE
CHo3z1eW64YjXn+/X9Z8QumnBGfocEPEZ4G1ZkMFJlAAAAQgD9JCT57f+yQzLU4ErwpFhC
nH0f/HHjfTAI5PO817vr0OKxkGcnJqzIeJ/LwiORPmNoXN+MdPLUgOWINu4uaj8f/AAAAA
podWxsLWVjNTIx
-----END OPENSSH PRIVATE KEY-----
`;

const EC256_ENC = `-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAACmFlczI1Ni1jdHIAAAAGYmNyeXB0AAAAGAAAABCmeF/Lnj
9/D1gMS3KRSkf6AAAAGAAAAAEAAABoAAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlz
dHAyNTYAAABBBNcglu8kPVlHCUJ3t1W26YdUDYRs2f7w/yq5xOM73CeRdHaDbjjZbggOpm
STmk3Jg4rJ2DPnzqtEyFRgXdpsgAMAAACw9LHNnNi0TWChZfTLq/N8XUws/ah0wVBc+S6R
OTO4K9RWpjGU5VOJi4XfiFA73qWQtoSPdg/N7FWC66L6SfYlwYbaVHAWF1lrlnk8iDGhF0
88+i3SPLQ531tPejab6R+J5KlfSH/q613Vra76nD5d6OgFY6FIGej/isEizTn3LZFV745f
0/Knab863Tw3Dw2Y0zpbDVvRpbyUIcYule+wVgs8kJVLJBeZLWd5UOI6+Nk=
-----END OPENSSH PRIVATE KEY-----
`;
const PASSPHRASE = "correct horse";

const MSG = "hull ssh ecdsa user key test";


// A stand-in: the self-check signs and verifies; `verifies` decides the answer.
const stub = (verifies) => ({ sign: () => "SIG", verify: () => verifies });

await test("an OpenSSH P-256 key loads, and its SEC1 PEM is OpenSSL's", () => {
    const k = privatekey.load(EC256, { crypto: stub(true) });
    assertEq(k.algorithm, "ecdsa-sha2-nistp256");
    assertEq(k.curve, "nistp256");
    assertEq(k.q.length, 65);
    assertEq(k.comment, "hull-ec256");
    assertEq(k.blob, EC256_PUB, "the blob a server is shown:");
    assertEq(k.pem, EC256_SEC1, "the PEM Hull builds:");
    assertEq(k.d, undefined, "the scalar is not kept outside the PEM:");
});

await test("an OpenSSH P-384 key loads, and its SEC1 PEM is OpenSSL's", () => {
    const k = privatekey.load(EC384, { crypto: stub(true) });
    assertEq(k.algorithm, "ecdsa-sha2-nistp384");
    assertEq(k.q.length, 97);
    assertEq(k.blob, EC384_PUB);
    assertEq(k.pem, EC384_SEC1);
});

await test("a P-521 key is refused with a code", () => {
    const [ok, err] = pcall(privatekey.load, EC521, { crypto: stub(true) });
    assertEq(ok, false);
    assertEq(typeof err === "object" && err.code, "unsupported_key_type");
});

await test("a key whose private half does not sign for its public point is damaged", () => {
    const [ok, err] = pcall(privatekey.load, EC256, { crypto: stub(false) });
    assertEq(ok, false);
    assertEq(errText(err).indexOf("ECDSA key is damaged") >= 0, true, errText(err));
});

await test("an ECDSA signature goes on the wire as mpint r, mpint s", () => {
    const r = "\x00" + "\x01".repeat(31);
    const s = "\x80" + "\x02".repeat(31);
    const blob = userauth.signatureBlob(r + s, "ecdsa-sha2-nistp256");
    const rd = wire.reader(blob);
    assertEq(rd.string(), "ecdsa-sha2-nistp256");
    const inner = wire.reader(rd.string());
    assertEq(inner.uint32(), 31);
    assertEq(inner.raw(31), "\x01".repeat(31));
    assertEq(inner.uint32(), 33);
    assertEq(inner.raw(33), "\x00" + s);
});

// against the real hull:crypto (bytes.crypto) ----------------------------------

await realTest("a P-256 key signs, and the signature verifies against ssh-keygen's export", () => {
    const k = privatekey.load(EC256);
    const sig = crypto.sign("ES256", k.pem, MSG);
    assertEq(sig.length, 64);
    assertEq(crypto.verify("ES256", EC256_SPKI, MSG, sig), true);
});

await realTest("a P-384 key signs, and the signature verifies", () => {
    const k = privatekey.load(EC384);
    const sig = crypto.sign("ES384", k.pem, MSG);
    assertEq(sig.length, 96);
    assertEq(crypto.verify("ES384", EC384_SPKI, MSG, sig), true);
});

await realTest("a passphrase-protected copy loads to the same key and signs", () => {
    const k = privatekey.load(EC256_ENC, { passphrase: PASSPHRASE });
    assertEq(k.blob, EC256_PUB);
    const sig = crypto.sign("ES256", k.pem, MSG);
    assertEq(crypto.verify("ES256", EC256_SPKI, MSG, sig), true);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
