// test_ssh_rsa_key.js - RSA user keys, against keys ssh-keygen wrote (JS twin
// of test_ssh_rsa_key.lua).
//
// USER_RSA is `ssh-keygen -t rsa -b 2048`; USER_RSA_ENC the same key after
// `ssh-keygen -p` with PASSPHRASE. SIG_512 / SIG_256 are OpenSSL's signatures
// over MSG with that key: RSA PKCS#1 v1.5 is deterministic, so Hull signing
// with the key it loaded must produce exactly those bytes.
//
// The Lua suite runs the structural half vanilla (3) and everything with the
// real hull.crypto (6). This file always has the capability layer: the
// stand-in cases and the real-crypto cases run in one pass (6).

import { privatekey } from "hull:ssh:privatekey";
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

const USER_RSA = `-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAABFwAAAAdzc2gtcn
NhAAAAAwEAAQAAAQEAsSTLYc8M/ovN3quKNnnHBq7+TStYfIWAqp0DEtjuJaCd8QsbxSda
JDx6kJCBMCVRJtr4SFEIUWn7vT3wU5aYkj9CVGpPa6cxBDkzgwsZXd9RVL+PeqcdVryR5f
BB0JmTveH74hg6af1twCkKxmWefb99aHvU56Iuo/0d/7KonbkjUfvZMXqe5n0otML0dDUP
be2Wr+v3TYu1sGJqOKgb3utlZAVyNsQh7sL/hjx9iGW012/rsB/hh1g+d1YTUItwv6KIfV
4y+libse08I6Sd2BejCgSsVCqUBscDpHQuMgCZrL4zP/znGv5Ckk+znoAjlgN3/ApfGMY4
iUm12G2KfwAAA8DgG2CQ4BtgkAAAAAdzc2gtcnNhAAABAQCxJMthzwz+i83eq4o2eccGrv
5NK1h8hYCqnQMS2O4loJ3xCxvFJ1okPHqQkIEwJVEm2vhIUQhRafu9PfBTlpiSP0JUak9r
pzEEOTODCxld31FUv496px1WvJHl8EHQmZO94fviGDpp/W3AKQrGZZ59v31oe9Tnoi6j/R
3/sqiduSNR+9kxep7mfSi0wvR0NQ9t7Zav6/dNi7WwYmo4qBve62VkBXI2xCHuwv+GPH2I
ZbTXb+uwH+GHWD53VhNQi3C/ooh9XjL6WJux7TwjpJ3YF6MKBKxUKpQGxwOkdC4yAJmsvj
M//Oca/kKST7OegCOWA3f8Cl8YxjiJSbXYbYp/AAAAAwEAAQAAAQASIq+1wEJWzw5RFSPN
xvLi0siTMvRYrzxIe1GkvNPIw2RYGKiLqyc13YE4yqqHONLRlQplMPKWjzCoI662iAzvsA
1E4itmXJQOmCKXcGv06deEQyZXysMcraqMLWg3iHmRL5EfiRZ14m25hholzKKIwjF/IGpg
GoEG5fOLyYksuGsyiTAb8m3VpcUvP01IMyC1P4LzJJ8OLvNpny3daJZrtu7xfC4CZb9A2G
g9gPYBpXC/RSzqm7gqgITu5h6I8gh53tP452Lrw/0eoyooExZcy+cpjTOTawUKGl6UhShJ
ow1MZUNnkCfMrPjOmIOeztyi6P+Pi7IC8z1xmUHM9awRAAAAgQCQWpX8kSfNTWMvcreGzG
A5s5tTgRhedx7d8sDq5dQkhJxqQhN1pP3ins75n/RgDgRqdjf+J6fjN0vkmYmG3DbxVY5X
yXrPmlAvMST7c/J/2K6VBbEIxZ/JL2ynDci0tpCqdziICWf6Qa34f1Fz0g4rnD35zKXWdV
FXZTjaxhOrSwAAAIEA7VjTh7nLS6+KgdBjji0mHPxHmi74tyxlwvebsLFG4+H7oWu0pEDn
vSSxoBlCdbjDhZ0WT0OMNk+EgqJLFeFLf+Dg40FxjgVC0BpoK+NAe7LIfDm8BWIe1ItNSM
O243GKAT9Fchh3TYzK72b8J6oqv6tk21Pbxm1d3XCodhGmgFEAAACBAL8QvddxcfE2AlIm
REBQPPI9I5DEgCr65dNCpTTI5LnWKGGcIPpGOW22yyU+MO9ZhvcOUoHRwiGHH/g3IxZY42
/8mdsedIU+sHXaZPbLCV2Jx6VuppWjQKHZs9BTO6sQANBYdxByYSEH3a/dJ2SuSCv4mSNI
4WgrT+v1D9hF7fnPAAAACWh1bGwtdGVzdAE=
-----END OPENSSH PRIVATE KEY-----
`;

const USER_RSA_ENC = `-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAACmFlczI1Ni1jdHIAAAAGYmNyeXB0AAAAGAAAABC9Ag6UQ7
p5gBH+tcI3PO7IAAAAGAAAAAEAAAEXAAAAB3NzaC1yc2EAAAADAQABAAABAQCxJMthzwz+
i83eq4o2eccGrv5NK1h8hYCqnQMS2O4loJ3xCxvFJ1okPHqQkIEwJVEm2vhIUQhRafu9Pf
BTlpiSP0JUak9rpzEEOTODCxld31FUv496px1WvJHl8EHQmZO94fviGDpp/W3AKQrGZZ59
v31oe9Tnoi6j/R3/sqiduSNR+9kxep7mfSi0wvR0NQ9t7Zav6/dNi7WwYmo4qBve62VkBX
I2xCHuwv+GPH2IZbTXb+uwH+GHWD53VhNQi3C/ooh9XjL6WJux7TwjpJ3YF6MKBKxUKpQG
xwOkdC4yAJmsvjM//Oca/kKST7OegCOWA3f8Cl8YxjiJSbXYbYp/AAADwBpUNxvuCOT5jk
bL04AtHj21x7D6kgg1AHNxb6XD6WZVIT9G3u87jdPWllo8HDmqLNnEBXUJOvqQ8JRLSp0Y
eG7vApUvSUI0zYZlRGcHnQfrCFBsixHOvvudWhoPZiScpy9CWOy5KZcNYpufx8zneNjdva
C91ZmoIzRFXW+WLEP0qaDDRCkJ8044gTww3uxTDAlnut2Y+IzHuTJSg1duPOD1hHwLFsTg
DcDmkEqJkT3OBWxS0egy+wXTQXBl4uJL4LzPSS+wAWVOsFBmQjKMAXo6t3gHUQxLw4YUnz
/Lrio8nf/6ZpJyi6OY3PFlLfUVeviI9oC+Of4Kw3rZMCaTh2RX9kTy/XtFOb1zjAzSnfwM
9y9A9o+rAyAbUHJMsZxMx1bL0UA8HiePsRYpJSqLcyQedh9+qAIvTgqawEupSzwpGQbLDl
DN298RK991eMiX1IY9QnWfJRSP86Yko9uJuk8g08Qrc50EbCZsB+S47kTj3g4dd1ZoYoss
gaPpUXuyRBFyqH0id9YBI1lSB7bdCl//XraeXmkEwhr3MCB9+cXUgwe2czyVurmMSP78RP
nVFXDSckMsA47mhfFJ0v0alQhJI+DkJfhr0HdD7PE+ZA1Thq+XTfMcMcAuBXIA1i/MweoH
Oi2gP1n5WjwkbFBeaYFesTCAkZubtMC9tSteTG3nde2nDo25qc8b3ZegbPF59KLi2vgLfk
ncGhy0GGm9LzbsOZQD5Al5FhxSjp4qw+uOz7yDjg6ePu+TOnd8dWpAE78ZqbXuLz3yoFjM
TKnQfPpWK9acCgaeX7b7GqLs48vLwHCMsJcpWMc4kKZ2baMJJY6fo5yuO+njMOF35eqkib
kjc8zS0VAr1lnltYnwrxozcYLVGBlF3sQsnS4c+9bKGcNu0hPDFAcebz3Q7IWN/qEMCN7J
8TZAMWu8ZNuW4K3TZwsBNPhhflC2Xeg+bh3HZe5uykHP0x+86z2r/ITaXTSCzfgpmqOsRC
rXva6umvkLcDaALyEI+VcEArhq1gMyUPmLZaCjYZIn9m2EgqVXSiSuO4IQ6Fav0TvbLXpY
qqT0zZNM2sbrfxmtkZHhGN72rzTwo4rzLig5uph+r0Si1bk2iAEsUTuEGcn34tJCEuX2Uk
vuUSqNOmM6ncTsO2EHTFuWyPz2Jp6imY3iFObXW8sXvaxOMMnohAXtrcyKQX07zGM+MI0i
p1HTR0BAJNCB7hfzpaksTgw3bsLeITrJ2SK7R/PBDgkkF2VEpS+HUtLHxbCBXYsyIlfC/a
LUIeBUrg==
-----END OPENSSH PRIVATE KEY-----
`;
const PASSPHRASE = "correct horse";

const USER_RSA_1024 = `-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAlwAAAAdzc2gtcn
NhAAAAAwEAAQAAAIEAtftrWXB2+2LSMx5J7abZvP5LSUGkeW7xA2WplOqyyhF/38j4m0U/
0F7l5L5PNWpWCBNlJ5jdKZKPJiGGBfUN+AAlk6sUp/rsceDdn8TreMQzo7Sfu2rDIHcsRH
dVn0qocVeATPMCd+WWVK5WqPIWJ9AR2+dkNd6Pmm0NgzfUg9cAAAH4OOMg7TjjIO0AAAAH
c3NoLXJzYQAAAIEAtftrWXB2+2LSMx5J7abZvP5LSUGkeW7xA2WplOqyyhF/38j4m0U/0F
7l5L5PNWpWCBNlJ5jdKZKPJiGGBfUN+AAlk6sUp/rsceDdn8TreMQzo7Sfu2rDIHcsRHdV
n0qocVeATPMCd+WWVK5WqPIWJ9AR2+dkNd6Pmm0NgzfUg9cAAAADAQABAAAAgFW7s9A4Dq
XR4YtZmqSOdYf3GUrS1QSsgnkCPQk+JKrT4bVw/cZQsoadHptMGQ7mIM+/K2mjN5YtSKlD
I9vMnh5Lf+40KUP+Ob4v2pwnZDrt8LpIe37u/H4qI3jgdSBScbwudri8iXEJGrXVzPrLwb
L+gJ0oaEK2e0BvBhyv61wZAAAAQFvvJNso1U3stc8XIgamBTcZrfE7Knwi7kgAxN0Bpm37
Nr1liTHIwLE/70oKjxJhL34+/rxAIUeAbdEhEMnla48AAABBAOJ5sZgHDySQABkrs7Guve
QRIKbH6JtutzpvLlR+yBGz7EwAeBGIl+URITkZvb261YWS1KExeMQZuO+VGUz+iZsAAABB
AM200uRVvXTyTTOJMbtk13JVVkL8L8DqYu40Xio3+BoRoD9nqQIgY28wZWS27Fe3QngaSh
6+aHDmTlJw9krI4HUAAAAAAQID
-----END OPENSSH PRIVATE KEY-----
`;

const PUB_BLOB = base64.decode("AAAAB3NzaC1yc2EAAAADAQABAAABAQCxJMthzwz+i83eq4o2eccGrv5NK1h8hYCqnQMS2O4loJ3xCxvFJ1okPHqQkIEwJVEm2vhIUQhRafu9PfBTlpiSP0JUak9rpzEEOTODCxld31FUv496px1WvJHl8EHQmZO94fviGDpp/W3AKQrGZZ59v31oe9Tnoi6j/R3/sqiduSNR+9kxep7mfSi0wvR0NQ9t7Zav6/dNi7WwYmo4qBve62VkBXI2xCHuwv+GPH2IZbTXb+uwH+GHWD53VhNQi3C/ooh9XjL6WJux7TwjpJ3YF6MKBKxUKpQGxwOkdC4yAJmsvjM//Oca/kKST7OegCOWA3f8Cl8YxjiJSbXYbYp/");

const SPKI = `-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAsSTLYc8M/ovN3quKNnnH
Bq7+TStYfIWAqp0DEtjuJaCd8QsbxSdaJDx6kJCBMCVRJtr4SFEIUWn7vT3wU5aY
kj9CVGpPa6cxBDkzgwsZXd9RVL+PeqcdVryR5fBB0JmTveH74hg6af1twCkKxmWe
fb99aHvU56Iuo/0d/7KonbkjUfvZMXqe5n0otML0dDUPbe2Wr+v3TYu1sGJqOKgb
3utlZAVyNsQh7sL/hjx9iGW012/rsB/hh1g+d1YTUItwv6KIfV4y+libse08I6Sd
2BejCgSsVCqUBscDpHQuMgCZrL4zP/znGv5Ckk+znoAjlgN3/ApfGMY4iUm12G2K
fwIDAQAB
-----END PUBLIC KEY-----
`;

const MSG = "hull ssh rsa user key test";
const SIG_512 = base64.decode("leR60R0LMuWFhh51n//J2gEL7KdQeJWYuyeKyvaBRsnjlhPx/t1qMlv1srVvXg0ckAkYrhOiqyrJ3g++5qqVxjBxpYkzLy++qevhgaj47YXD81CeHcBmGE6Belf+1TClogU4x+cTRFN0tMUdDnY0LgDiVXiM0/WozMXuWpU3VbSZ/UfHEsSU8Q1E0tZKKmX6dCuRajmaIxWjgyBjkrfax0tid88AIfzmKtHYjk2UkPuiIBATHbDCWCo7rtKFJuK/LwxxcTHvoMr5Y699Bt5cbHNJove+sBYgh4ylBwsnk73/n1tx9b+fy4NOV8FFLS5zqZVtqYJilCWkhYNTZSn7NA==");
const SIG_256 = base64.decode("Bo+oPkr2i7urk13+dpF/Ai9/QO+ioCg/EpgK/aBrNuL0t4XHHHVkhTpUM8B0PASV/8dT9emZL955+/DacQv+J9+/KRmQgOCda0dbHN8lpM93tM6I741+Opv4YZPjcalomNzjOj6QVRmIE2Zxkemfmq6JPhpMf4TWyxjeOsZ9zuWH9dF3es4dzYpos6pg6bFGSeqpZ/i4ev6RJksUqsSR8ABToY9OMAsysF86j7ISkHCQEhcxVsjQrRDav/tmBhvrx/kbZB85Mcdkd4ha2lXWShSvOvNEC6CO4+dY6HPMA9thFmWsEi/BowkFFMPWFUjIQE27986qo5J2QM0l/r6kZg==");


// A stand-in that records what it was asked to build.
function stub(failWith) {
    const seen = {};
    return [{
        rsaPrivatePem: (n, e, d, p, q) => {
            if (failWith) throw new Error(failWith);
            seen.n = n; seen.e = e; seen.d = d; seen.p = p; seen.q = q;
            return "PEM";
        },
    }, seen];
}

await test("an OpenSSH RSA key loads, and is presented by its public blob", () => {
    const [c, seen] = stub();
    const k = privatekey.load(USER_RSA, { crypto: c });
    assertEq(k.algorithm, "ssh-rsa");
    assertEq(k.bits, 2048);
    assertEq(k.comment, "hull-test");
    assertEq(k.blob, PUB_BLOB, "the blob a server is shown:");
    assertEq(k.pem, "PEM");
    assertEq(seen.n.length, 256);
    assertEq(seen.e, "\x01\x00\x01");
    assertEq(k.d, undefined);
    assertEq(k.p, undefined);
    assertEq(k.q, undefined);
});

await test("an RSA key below 2048 bits is refused with a code", () => {
    const [ok, err] = pcall(privatekey.load, USER_RSA_1024, { crypto: stub()[0] });
    assertEq(ok, false);
    assertEq(typeof err === "object" && err.code, "unsupported_key_type");
    assertEq(errText(err).indexOf("2048") >= 0, true, errText(err));
});

await test("components that do not form a key are reported as a damaged key", () => {
    const [ok, err] = pcall(privatekey.load, USER_RSA,
        { crypto: stub("the components do not form a valid RSA key")[0] });
    assertEq(ok, false);
    assertEq(errText(err).indexOf("RSA key is damaged") >= 0, true, errText(err));
});

// against the real hull:crypto (bytes.crypto) ----------------------------------

await realTest("the loaded key signs exactly as OpenSSL does with the same key", () => {
    const k = privatekey.load(USER_RSA);
    assertEq(crypto.sign("RS512", k.pem, MSG), SIG_512, "RS512");
    assertEq(crypto.sign("RS256", k.pem, MSG), SIG_256, "RS256");
});

await realTest("a passphrase-protected RSA key loads to the same key", () => {
    const k = privatekey.load(USER_RSA_ENC, { passphrase: PASSPHRASE });
    assertEq(k.blob, PUB_BLOB);
    assertEq(crypto.sign("RS512", k.pem, MSG), SIG_512);
});

await realTest("its signature verifies against the public key ssh-keygen exported", () => {
    const k = privatekey.load(USER_RSA);
    assertEq(crypto.verify("RS512", SPKI, MSG, crypto.sign("RS512", k.pem, MSG)), true);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
