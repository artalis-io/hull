// hull:ssh:hostkey - host key parsing, fingerprints, and the trust decision.
//
// The JS twin of hull.ssh.hostkey (stdlib/lua/hull/ssh/hostkey.lua); see
// there for the reasoning behind each rule. This is the module that decides
// whether Hull is talking to the machine it meant to talk to.
//
// The key is exposed BEFORE trust (verify reports a status and decides
// nothing), and the store is the APPLICATION's: an object with get / put /
// forget (and optionally revoked), whose methods may be sync or async - every
// call to one is awaited, so check / acceptNew / forget / verify are async.
//
// Byte strings throughout (hull:ssh:bytes); positions are 0-based.

import { wire } from "hull:ssh:wire";
import { encoding } from "hull:encoding";

const base64 = encoding.base64;

const errText = (e) => (e && typeof e === "object" && typeof e.message === "string")
    ? e.message : String(e);

const ALGORITHM = "ssh-ed25519";
const KEY_LEN = 32;
const SIG_LEN = 64;

// Every host key algorithm Hull verifies, by the name negotiated in KEXINIT.
// `keyType` is the name inside the key blob; `jose` the crypto.verify name;
// `field` the curve's coordinate size.
const ALGORITHMS = {
    "ssh-ed25519":         { keyType: "ssh-ed25519" },
    "ecdsa-sha2-nistp256": { keyType: "ecdsa-sha2-nistp256", curve: "nistp256",
                             jose: "ES256", field: 32 },
    "ecdsa-sha2-nistp384": { keyType: "ecdsa-sha2-nistp384", curve: "nistp384",
                             jose: "ES384", field: 48 },
    "rsa-sha2-512":        { keyType: "ssh-rsa", jose: "RS512" },
    "rsa-sha2-256":        { keyType: "ssh-rsa", jose: "RS256" },
};

const hasOwn = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const algo = (name) => (typeof name === "string" && hasOwn(ALGORITHMS, name))
    ? ALGORITHMS[name] : null;

// The algorithms a key of `keyType` can be negotiated under, best first.
function algorithmsFor(keyType) {
    if (keyType === "ssh-rsa") return ["rsa-sha2-512", "rsa-sha2-256"];
    if (algo(keyType)) return [keyType];
    return [];
}

// The key type a blob declares (its first string), or null if it has none.
function keyType(blob) {
    try { return wire.reader(blob).string(); } catch (e) { return null; }
}

const RSA_MIN_BITS = 2048;
const RSA_MAX_BITS = 16384;

// DER, for the PEM that crypto.verify reads ---------------------------

function unhex(h) {
    let s = "";
    for (let i = 0; i < h.length; i += 2) s += String.fromCharCode(parseInt(h.slice(i, i + 2), 16));
    return s;
}

function der(tag, body) {
    let n = body.length;
    let len;
    if (n < 0x80) {
        len = String.fromCharCode(n);
    } else {
        const b = [];
        while (n > 0) { b.unshift(String.fromCharCode(n & 0xFF)); n = Math.floor(n / 256); }
        len = String.fromCharCode(0x80 | b.length) + b.join("");
    }
    return String.fromCharCode(tag) + len + body;
}

// A DER INTEGER from an unsigned big-endian magnitude.
function derUint(mag) {
    if (mag === "") mag = "\x00";
    if (mag.charCodeAt(0) >= 0x80) mag = "\x00" + mag;
    return der(0x02, mag);
}

const EC_SPKI_PREFIX = {
    nistp256: unhex("3059301306072a8648ce3d020106082a8648ce3d030107034200"),
    nistp384: unhex("3076301006072a8648ce3d020106052b81040022036200"),
};
const RSA_ALG_ID = unhex("300d06092a864886f70d0101010500");
const EC_CURVE_OID = {
    nistp256: unhex("06082a8648ce3d030107"),
    nistp384: unhex("06052b81040022"),
};

function pem(derBytes, label) {
    label = label || "PUBLIC KEY";
    const b64 = base64.encode(derBytes);
    const lines = [];
    for (let i = 0; i < b64.length; i += 64) lines.push(b64.slice(i, i + 64));
    return "-----BEGIN " + label + "-----\n" + lines.join("\n")
        + "\n-----END " + label + "-----\n";
}

// Key and signature blobs -----------------------------------------------

function rsaBits(n) {
    let top = n.charCodeAt(0), bits = 0;
    while (top > 0) { bits++; top >>= 1; }
    return (n.length - 1) * 8 + bits;
}

// A host key blob. Returns { type, algorithm } plus `key` (Ed25519), `q`
// (ECDSA point) or `e` / `n` / `bits` (RSA).
function parseKey(blob) {
    const r = wire.reader(blob);
    const t = r.string();
    const out = { type: t, algorithm: t };
    if (t === ALGORITHM) {
        out.key = r.string();
        if (out.key.length !== KEY_LEN) {
            throw new Error("ssh.hostkey: Ed25519 key is " + String(out.key.length)
                + " bytes, expected 32");
        }
    } else if (t === "ecdsa-sha2-nistp256" || t === "ecdsa-sha2-nistp384") {
        const a = ALGORITHMS[t];
        const curve = r.string();
        if (curve !== a.curve) {
            throw new Error("ssh.hostkey: " + t + " key names the curve " + wire.safeName(curve));
        }
        out.q = r.string();
        if (out.q.length !== 1 + 2 * a.field || out.q.charCodeAt(0) !== 4) {
            throw new Error("ssh.hostkey: " + t + " point is not an uncompressed "
                + a.curve + " point");
        }
    } else if (t === "ssh-rsa") {
        out.e = r.mpintCanonical();   // one encoding per key (audit 44)
        out.n = r.mpintCanonical();
        if (out.n === "") throw new Error("ssh.hostkey: RSA modulus is zero");
        out.bits = rsaBits(out.n);
        if (out.bits < RSA_MIN_BITS) {
            throw new Error("ssh.hostkey: RSA host key of " + String(out.bits)
                + " bits is below the " + String(RSA_MIN_BITS) + "-bit minimum");
        }
        if (out.bits > RSA_MAX_BITS) {
            throw new Error("ssh.hostkey: RSA host key of " + String(out.bits)
                + " bits is above the " + String(RSA_MAX_BITS) + "-bit maximum");
        }
        if (out.e === "" || out.e === "\x01" || out.e.charCodeAt(out.e.length - 1) % 2 === 0) {
            throw new Error("ssh.hostkey: RSA public exponent must be odd and greater than 1");
        }
    } else {
        throw new Error("ssh.hostkey: unsupported host key algorithm: " + wire.safeName(t));
    }
    if (r.remaining() !== 0) {
        throw new Error("ssh.hostkey: trailing bytes after the " + t + " key");
    }
    return out;
}

// A signature blob; ECDSA comes back as the fixed-width r || s.
function parseSignature(blob) {
    const r = wire.reader(blob);
    const name = r.string();
    let sig = r.string();
    const a = algo(name);
    if (!a) {
        throw new Error("ssh.hostkey: unsupported signature algorithm: " + wire.safeName(name));
    }
    if (name === ALGORITHM) {
        if (sig.length !== SIG_LEN) {
            throw new Error("ssh.hostkey: Ed25519 signature is " + String(sig.length)
                + " bytes, expected 64");
        }
    } else if (a.field) {
        const rs = wire.reader(sig);
        const sr = rs.mpint(), ss = rs.mpint();
        if (rs.remaining() !== 0) {
            throw new Error("ssh.hostkey: trailing bytes in the " + name + " signature");
        }
        if (sr === "" || ss === "" || sr.length > a.field || ss.length > a.field) {
            throw new Error("ssh.hostkey: " + name + " signature values do not fit the curve");
        }
        sig = "\x00".repeat(a.field - sr.length) + sr + "\x00".repeat(a.field - ss.length) + ss;
    } else if (sig.length === 0) {
        throw new Error("ssh.hostkey: empty " + name + " signature");
    }
    if (r.remaining() !== 0) {
        throw new Error("ssh.hostkey: trailing bytes after the " + name + " signature");
    }
    return { algorithm: name, signature: sig };
}

// The PEM SubjectPublicKeyInfo of a parsed ECDSA or RSA key.
function publicKeyPem(key) {
    if (key.q) {
        return pem(EC_SPKI_PREFIX[ALGORITHMS[key.type].curve] + key.q);
    } else if (key.n) {
        const rsa = der(0x30, derUint(key.n) + derUint(key.e));
        return pem(der(0x30, RSA_ALG_ID + der(0x03, "\x00" + rsa)));
    }
    throw new Error("ssh.hostkey: a " + String(key.type) + " key has no PEM form");
}

// The SEC1 PEM ("EC PRIVATE KEY") of the ECDSA key (q, d). Secret.
function ecPrivatePem(curve, q, d) {
    const a = algo("ecdsa-sha2-" + String(curve));
    if (!a || !a.field || !hasOwn(EC_CURVE_OID, curve)) {
        throw new Error("ssh.hostkey: no ECDSA curve " + wire.safeName(String(curve)));
    }
    if (typeof q !== "string" || q.length !== 1 + 2 * a.field || q.charCodeAt(0) !== 4) {
        throw new Error("ssh.hostkey: not an uncompressed " + curve + " point");
    }
    if (typeof d !== "string" || d === "" || d.length > a.field) {
        throw new Error("ssh.hostkey: the ECDSA private scalar does not fit " + curve);
    }
    const body = der(0x02, "\x01")
        + der(0x04, "\x00".repeat(a.field - d.length) + d)
        + der(0xA0, EC_CURVE_OID[curve])
        + der(0xA1, der(0x03, "\x00" + q));
    return pem(der(0x30, body), "EC PRIVATE KEY");
}

// Fingerprints ------------------------------------------------------------

// SHA256:<unpadded standard base64>, what `ssh-keygen -l` prints.
// `sha256Raw` takes bytes and returns the 32 raw digest bytes.
function fingerprint(sha256Raw, blob) {
    if (typeof sha256Raw !== "function") {
        throw new TypeError("ssh.hostkey: fingerprint needs a sha256 function");
    }
    return "SHA256:" + base64.encode(sha256Raw(blob), { pad: false });
}

// Verification --------------------------------------------------------------

// Verify the server signature over the exchange hash under `alg` (the
// negotiated host key algorithm; the signature's own when absent).
// -> [true] | [false, reason]
function verifySignature(crypto, keyBlob, sigBlob, h, alg) {
    let key, sig;
    try { key = parseKey(keyBlob); } catch (e) { return [false, errText(e)]; }
    try { sig = parseSignature(sigBlob); } catch (e) { return [false, errText(e)]; }

    if (alg === undefined || alg === null) alg = sig.algorithm;
    const a = algo(alg);
    if (!a || sig.algorithm !== alg) {
        return [false, "ssh: host key signature is " + wire.safeName(sig.algorithm)
            + ", negotiated " + wire.safeName(String(alg))];
    }
    if (key.type !== a.keyType) {
        return [false, "ssh: a " + wire.safeName(key.type) + " key cannot sign under " + alg];
    }

    let good;
    if (alg === ALGORITHM) {
        good = crypto.ed25519Verify(h, sig.signature, key.key);
    } else {
        let s = sig.signature;
        if (key.n) {
            // RFC 8332 section 3: as long as the modulus; pad dropped zeros.
            if (s.length > key.n.length) {
                return [false, "ssh: RSA signature is longer than the modulus"];
            }
            s = "\x00".repeat(key.n.length - s.length) + s;
        }
        good = crypto.verify(a.jose, publicKeyPem(key), h, s);
    }
    if (good) return [true];
    return [false, "ssh: host key signature does not verify"];
}

// Trust ---------------------------------------------------------------------

const TRUSTED = "trusted";
const UNKNOWN = "unknown";
const CHANGED = "changed";
const REVOKED = "revoked";

// Lua's string.lower: ASCII only.
function asciiLower(s) {
    return s.replace(/[A-Z]+/g, (m) => m.toLowerCase());
}

// The name a host's key is stored under: OpenSSH's known_hosts convention.
function storeName(host, port) {
    const h = asciiLower(String(host));
    if (h === "" || /[,#|*?!\[\] \x00-\x1f\x7f]/.test(h)) {
        throw new TypeError("ssh.hostkey: invalid host name for the trust store");
    }
    if (port === undefined || port === null) port = 22;
    if (port === 22) return h;
    return "[" + h + "]:" + String(port);
}

// Compare the presented key against the store. -> [status, stored]
async function check(store, host, keyBlob) {
    if (!store || typeof store !== "object" || typeof store.get !== "function") {
        throw new TypeError("ssh.hostkey: a store with a get function is required");
    }
    // A pending trust (hull:ssh pendingTrust, platform gap 4): the FIRST key
    // presented is the one its single connection is held to, and nothing is
    // stored anywhere. Once it holds a key, only that key for that host is
    // trusted - a rekey is pinned by the transport as well, and a second
    // connect never gets this far (ssh.connect refuses a used one).
    if (store.pending === true) {
        if (store.key === null || store.key === undefined) {
            store.key = keyBlob;
            store.name = host;
            return [TRUSTED, keyBlob];
        }
        if (store.key === keyBlob && store.name === host) return [TRUSTED, store.key];
        return [CHANGED, store.key];
    }
    if (typeof store.revoked === "function" && await store.revoked(keyBlob)) {
        return [REVOKED, null];
    }
    let stored = await store.get(host);
    if (stored === undefined) stored = null;
    if (stored === null) return [UNKNOWN, null];
    if (stored === keyBlob) return [TRUSTED, stored];
    return [CHANGED, stored];
}

// Record a key for a host not seen before. Refuses to overwrite.
async function acceptNew(store, host, keyBlob) {
    if (typeof store.put !== "function") {
        throw new TypeError("ssh.hostkey: the store has no put function");
    }
    const cur = await store.get(host);
    if (cur !== undefined && cur !== null) {
        throw new Error("ssh.hostkey: " + String(host)
            + " already has a stored key; forget it first");
    }
    await store.put(host, keyBlob);
    return true;
}

// Drop a stored key, so a genuinely rotated host can be re-accepted.
async function forget(store, host) {
    if (typeof store.forget !== "function") {
        throw new TypeError("ssh.hostkey: the store has no forget function");
    }
    await store.forget(host);
    return true;
}

// The whole host-key step: verify the signature, then report trust.
//   { ok: false, reason }
//   { ok: true, status, fingerprint, keyBlob [, storedFingerprint] }
async function verify(crypto, store, host, keyBlob, sigBlob, h, alg) {
    const [ok, reason] = verifySignature(crypto, keyBlob, sigBlob, h, alg);
    if (!ok) return { ok: false, reason };

    const [status, stored] = await check(store, host, keyBlob);
    const out = {
        ok: true,
        status,
        fingerprint: fingerprint(crypto.sha256, keyBlob),
        keyBlob,
    };
    if (status === CHANGED) out.storedFingerprint = fingerprint(crypto.sha256, stored);
    return out;
}

export const hostkey = {
    ALGORITHM, KEY_LEN, SIG_LEN, ALGORITHMS, RSA_MIN_BITS, RSA_MAX_BITS,
    TRUSTED, UNKNOWN, CHANGED, REVOKED,
    algorithmsFor, keyType, parseKey, parseSignature, publicKeyPem, ecPrivatePem,
    fingerprint, verifySignature, storeName, check, acceptNew, forget, verify,
};
export default hostkey;
