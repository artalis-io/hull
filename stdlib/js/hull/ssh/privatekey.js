// hull:ssh:privatekey - load an OpenSSH private key file.
//
// The JS twin of hull.ssh.privatekey (stdlib/lua/hull/ssh/privatekey.lua);
// see there for the format notes. Parses the `openssh-key-v1` container;
// encrypted keys are decrypted (aes256-ctr + bcrypt only) when the caller says
// where the passphrase comes from, and refused by name otherwise.
//
// WHAT IS AND IS NOT PROTECTED. `passphraseEnv` keeps the PASSPHRASE out of
// JS entirely: C reads the variable, derives, and scrubs its buffers. The
// derived key and the decrypted private key are ordinary JS strings, which
// nothing can scrub - load a key once at the point of use.
//
// Byte strings throughout (hull:ssh:bytes). load is synchronous.

import { wire } from "hull:ssh:wire";
import { encoding } from "hull:encoding";
import { bytes } from "hull:ssh:bytes";
import { hostkey } from "hull:ssh:hostkey";

const base64 = encoding.base64;

const errText = (e) => (e && typeof e === "object" && typeof e.message === "string")
    ? e.message : String(e);

function sshError(code, detail, extra) {
    const e = new Error(detail === undefined || detail === null ? code : String(detail));
    e.code = code;
    if (detail !== undefined) e.detail = detail;
    if (extra) for (const k of Object.keys(extra)) e[k] = extra[k];
    // The connection's own failure, never a runtime refusal (hull:_refusal).
    Object.defineProperty(e, "ssh", { value: true, enumerable: false });
    return e;
}

const MAGIC = "openssh-key-v1\x00";
const BEGIN = "-----BEGIN OPENSSH PRIVATE KEY-----";
const END = "-----END OPENSSH PRIVATE KEY-----";
const ALGORITHM = "ssh-ed25519";
const RSA = "ssh-rsa";
const RSA_MIN_BITS = 2048;

const ECDSA = {
    "ecdsa-sha2-nistp256": "nistp256",
    "ecdsa-sha2-nistp384": "nistp384",
};
const ecdsaCurve = (a) => (typeof a === "string"
    && Object.prototype.hasOwnProperty.call(ECDSA, a)) ? ECDSA[a] : null;

// The failures a caller acts on differently carry a code.
function raise(code, msg) {
    throw sshError(code, msg);
}

// Strip the armour and decode the body.
function unarmour(text) {
    if (typeof text !== "string") {
        throw new TypeError("ssh.privatekey: expected the file contents as a string");
    }
    const b = text.indexOf(BEGIN);
    if (b < 0) {
        const msg = "ssh.privatekey: not an OpenSSH private key "
            + "(no " + BEGIN + " line); PEM and PKCS#8 keys are not supported";
        if (text.indexOf("-----BEGIN ") >= 0) raise("unsupported_key_type", msg);
        throw new Error(msg);
    }
    const e = text.indexOf(END, b);
    if (e < 0) throw new Error("ssh.privatekey: truncated key file (no END line)");
    const raw = base64.decode(text.slice(b + BEGIN.length, e), { lenient: true });
    if (raw === null || raw === undefined) {
        throw new Error("ssh.privatekey: damaged key file (invalid base64)");
    }
    return raw;
}

// Parse the container, without interpreting the key blob.
function parseContainer(raw) {
    if (raw.slice(0, MAGIC.length) !== MAGIC) {
        throw new Error("ssh.privatekey: bad magic; this is not an openssh-key-v1 file");
    }
    const r = wire.reader(raw.slice(MAGIC.length));
    const out = {};
    out.cipher = r.string();
    out.kdf = r.string();
    out.kdfopts = r.string();
    out.nkeys = r.uint32();
    if (out.nkeys !== 1) {
        throw new Error("ssh.privatekey: file holds " + String(out.nkeys)
            + " keys, expected exactly 1");
    }
    out.publicBlob = r.string();
    out.privateBlob = r.string();
    out.encrypted = out.cipher !== "none" || out.kdf !== "none";
    return out;
}

// Parse the private section. Only valid for an unencrypted key.
function parsePrivate(blob) {
    const r = wire.reader(blob);
    const check1 = r.uint32();
    const check2 = r.uint32();
    if (check1 !== check2) {
        throw new Error("ssh.privatekey: check bytes do not match; the file is corrupt");
    }

    const algo = r.string();
    let key;
    if (algo === ALGORITHM) {
        const pub = r.string();
        const secret = r.string();
        if (pub.length !== 32) {
            throw new Error("ssh.privatekey: public half is " + String(pub.length)
                + " bytes, expected 32");
        }
        if (secret.length !== 64) {
            throw new Error("ssh.privatekey: secret half is " + String(secret.length)
                + " bytes, expected 64");
        }
        if (secret.slice(32) !== pub) {
            throw new Error("ssh.privatekey: the two copies of the public key disagree");
        }
        key = { algorithm: algo, public: pub, secret };
    } else if (algo === RSA) {
        // n, e, d, iqmp, p, q; iqmp is dropped.
        const n = r.mpint(), e = r.mpint(), d = r.mpint();
        r.mpint();
        const p = r.mpint(), q = r.mpint();
        key = { algorithm: algo, n, e, d, p, q };
    } else if (ecdsaCurve(algo)) {
        const curve = r.string(), q = r.string(), d = r.mpint();
        if (curve !== ecdsaCurve(algo)) {
            throw new Error("ssh.privatekey: " + algo + " key names the curve "
                + wire.safeName(curve));
        }
        key = { algorithm: algo, curve, q, d };
    } else if (algo === "ecdsa-sha2-nistp521") {
        raise("unsupported_key_type", "ssh.privatekey: ecdsa-sha2-nistp521 keys are not "
            + "supported (P-256 and P-384 are); generate one with ssh-keygen -t ed25519");
    } else {
        raise("unsupported_key_type", "ssh.privatekey: unsupported key type "
            + wire.safeName(String(algo))
            + "; ssh-ed25519, ssh-rsa and ecdsa-sha2-nistp256/384 are supported");
    }

    const comment = r.string();

    // Padding is 1,2,3... to the cipher block size.
    let i = 1;
    while (r.remaining() > 0) {
        const b = r.byte();
        if (b !== (i & 0xFF)) {
            throw new Error("ssh.privatekey: bad padding at offset " + String(i));
        }
        i++;
    }

    key.comment = comment;
    return key;
}

function rsaBits(n) {
    let top = n.length ? n.charCodeAt(0) : 0, bits = 0;
    while (top > 0) { bits++; top >>= 1; }
    return (n.length - 1) * 8 + bits;
}

// An RSA key's public blob and signing PEM; the components are dropped.
function finishRsa(key, opts) {
    const bits = rsaBits(key.n);
    if (bits < RSA_MIN_BITS) {
        raise("unsupported_key_type", "ssh.privatekey: RSA key of " + String(bits)
            + " bits is below the " + String(RSA_MIN_BITS)
            + "-bit minimum; generate a new one (ssh-keygen -t ed25519)");
    }
    const crypto = opts.crypto || bytes.crypto;
    let p;
    try {
        p = crypto.rsaPrivatePem(key.n, key.e, key.d, key.p, key.q);
    } catch (e) {
        throw new Error("ssh.privatekey: the RSA key is damaged (" + errText(e) + ")");
    }
    key.pem = p;
    key.bits = bits;
    key.public = wire.writer().string(RSA).mpint(key.e).mpint(key.n).build();
    delete key.d;
    delete key.p;
    delete key.q;
}

const ECDSA_CHECK = "hull.ssh.privatekey: does this key sign for its own public point?";

// An ECDSA key's public blob and signing PEM, checked by signing a fixed
// message and verifying it against the file's own public point.
function finishEcdsa(key, opts) {
    key.public = wire.writer().string(key.algorithm).string(key.curve).string(key.q).build();
    let parsed;
    try { parsed = hostkey.parseKey(key.public); } catch (e) {
        throw new Error("ssh.privatekey: " + errText(e));
    }
    let p;
    try { p = hostkey.ecPrivatePem(key.curve, key.q, key.d); } catch (e) {
        throw new Error("ssh.privatekey: the ECDSA key is damaged (" + errText(e) + ")");
    }
    delete key.d;

    const crypto = opts.crypto || bytes.crypto;
    const jose = hostkey.ALGORITHMS[key.algorithm].jose;
    let sig, sok = true;
    try { sig = crypto.sign(jose, p, ECDSA_CHECK); } catch (e) { sok = false; }
    if (!sok || !crypto.verify(jose, hostkey.publicKeyPem(parsed), ECDSA_CHECK, sig)) {
        throw new Error("ssh.privatekey: the ECDSA key is damaged (its private half does not "
            + "match its public point)");
    }
    key.pem = p;
}

const CIPHER = "aes256-ctr";
const KDF = "bcrypt";
const CIPHER_KEY_LEN = 32;
const CIPHER_IV_LEN = 16;

// Decrypt the private section of a passphrase-protected key.
//   opts.passphraseEnv = "VAR"  the variable's NAME (preferred; stays in C)
//   opts.passphrase    = "..."  the passphrase itself
function decryptPrivate(container, opts) {
    opts = opts || {};
    if (container.cipher !== CIPHER || container.kdf !== KDF) {
        raise("unsupported_key_type", "ssh.privatekey: unsupported protection (cipher "
            + wire.safeName(String(container.cipher)) + ", kdf "
            + wire.safeName(String(container.kdf))
            + "); Hull reads " + CIPHER + " with " + KDF
            + ". Convert it with: ssh-keygen -p -f <file>");
    }

    // Lua truthiness: "" counts as given here, and is refused just below.
    const given = (v) => v !== undefined && v !== null && v !== false;
    if (!given(opts.passphraseEnv) && !given(opts.passphrase)) {
        raise("passphrase_required", "ssh.privatekey: the key is encrypted; pass a passphrase as "
            + "opts.passphraseEnv = \"VAR\" (preferred - the value never "
            + "becomes a JS string) or opts.passphrase = \"...\"");
    }
    if (opts.passphrase === "") {
        raise("passphrase_required", "ssh.privatekey: the key is encrypted but the passphrase is empty");
    }

    let salt, rounds;
    try {
        const ko = wire.reader(container.kdfopts);
        salt = ko.string();
        rounds = ko.uint32();
    } catch (e) {
        throw new Error("ssh.privatekey: the key declares " + KDF + " but its "
            + "kdfoptions are malformed (expected a salt and a round count)");
    }
    if (salt.length === 0) throw new Error("ssh.privatekey: the key carries an empty KDF salt");
    if (rounds === 0) throw new Error("ssh.privatekey: the key declares zero KDF rounds");

    const crypto = opts.crypto || bytes.crypto;

    const max = crypto.BCRYPT_MAX_ROUNDS;
    if (max && rounds > max) {
        throw new Error("ssh.privatekey: the key declares " + String(rounds)
            + " KDF rounds, above the " + String(max) + " limit "
            + "(ssh-keygen writes 16, or 24 with -a); refusing to derive");
    }

    const want = CIPHER_KEY_LEN + CIPHER_IV_LEN;

    // passphraseEnv wins when both are given.
    let derived = given(opts.passphraseEnv)
        ? crypto.bcryptPbkdfEnv(opts.passphraseEnv, salt, rounds, want)
        : crypto.bcryptPbkdf(opts.passphrase, salt, rounds, want);

    const plain = crypto.aes256ctr(derived.slice(0, CIPHER_KEY_LEN),
                                   derived.slice(CIPHER_KEY_LEN, want),
                                   container.privateBlob);
    derived = null;
    return plain;
}

// Load a key from the contents of a key file.
//
// Returns { algorithm, comment, blob } plus what signing needs:
//   ssh-ed25519  `public` (raw 32 bytes), `secret` (64)
//   ssh-rsa      `pem`, `bits`, `n`, `e`, `public` (the blob)
//   ecdsa-sha2-nistp256/384  `pem`, `curve`, `q`, `public` (the blob)
function load(text, opts) {
    const container = parseContainer(unarmour(text));
    let blob = container.privateBlob;
    if (container.encrypted) blob = decryptPrivate(container, opts);
    let key;
    try {
        key = parsePrivate(blob);
    } catch (e) {
        if (container.encrypted) {
            raise("bad_passphrase", "ssh.privatekey: wrong passphrase (or the key is damaged)");
        }
        throw e;
    }

    let expect;
    if (key.algorithm === RSA) {
        finishRsa(key, opts || {});
        expect = key.public;
    } else if (ecdsaCurve(key.algorithm)) {
        finishEcdsa(key, opts || {});
        expect = key.public;
    } else {
        expect = wire.writer().string(key.algorithm).string(key.public).build();
    }
    if (container.publicBlob !== expect) {
        throw new Error("ssh.privatekey: the public blob does not match the private key");
    }
    key.blob = container.publicBlob;
    return key;
}

// The fingerprint of the key's public blob.
function fingerprint(sha256Raw, key) {
    return hostkey.fingerprint(sha256Raw, key.blob);
}

export const privatekey = {
    MAGIC, BEGIN, END, ALGORITHM, RSA, RSA_MIN_BITS, ECDSA, CIPHER, KDF,
    unarmour, parseContainer, decryptPrivate, load, fingerprint,
};
export default privatekey;
