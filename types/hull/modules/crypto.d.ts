// hull:crypto - hashing, MACs, password hashing, signatures, public-key and
// secret-key encryption, randomness. Bytes in, ArrayBuffers out; never hex.
declare module "hull:crypto" {
    /**
     * Bytes a crypto binding takes: a buffer (ArrayBuffer, typed array, WasmBuffer,
     * MappedBuffer, Image pixels), or a string taken as its UTF-8 text. Pass a byte
     * string through `encoding.bytes.toU8` first: as a string it would be UTF-8 encoded.
     */
    export type CryptoBytes = string | Hull.Bytes | Hull.Image;

    /** The algorithms `crypto.sign` / `crypto.verify` take (ECDSA signatures are raw r||s). */
    export type AsymAlg = "RS256" | "RS384" | "RS512" | "PS256" | "ES256" | "ES384";

    /** A keypair as raw bytes. */
    export interface Keypair {
        /** The public key. */
        publicKey: ArrayBuffer;
        /** The secret key (Ed25519: 64 bytes; X25519 / box: 32 bytes). */
        secretKey: ArrayBuffer;
    }

    /** The result of `crypto.gcmSeal`. */
    export interface GcmSealed {
        /** The ciphertext, as long as the plaintext. */
        ciphertext: ArrayBuffer;
        /** The 16-byte authentication tag. */
        tag: ArrayBuffer;
    }

    /** An incremental SHA-256 (`crypto.createSha256()`). */
    export interface Sha256Hasher {
        /** Feed more data; returns the hasher. Throws after `digest()`. */
        update(data: CryptoBytes): Sha256Hasher;
        /** The 32-byte digest. Callable once. */
        digest(): ArrayBuffer;
    }

    /** A 32-byte secretbox key held in C memory (`crypto.keyFromEnv`); its bytes are never a JS value. */
    export interface CryptoKey {
        /** Seal `message` under this key with a 24-byte nonce: ciphertext with its 16-byte tag. */
        secretbox(message: CryptoBytes, nonce: CryptoBytes): ArrayBuffer;
        /** Open a secretbox ciphertext; null when it does not authenticate. */
        secretboxOpen(ciphertext: CryptoBytes, nonce: CryptoBytes): ArrayBuffer | null;
        /** Zero the key now (idempotent); later use throws. */
        destroy(): void;
        /** `crypto.key(NAME)`, never the key bytes. */
        toString(): string;
    }

    /** Cryptographic primitives. */
    export const crypto: {
        /** SHA-256 digest (32 bytes). */
        readonly sha256: (data: CryptoBytes) => ArrayBuffer;
        /** A 32-byte secretbox key read from an environment variable in `manifest.env` (64 hex digits or base64). */
        readonly keyFromEnv: (name: string) => CryptoKey;
        /** An incremental SHA-256 hasher. */
        readonly createSha256: () => Sha256Hasher;
        /** SHA-512 digest (64 bytes). */
        readonly sha512: (data: CryptoBytes) => ArrayBuffer;
        /** SHA-1 digest (20 bytes). Legacy interop only. */
        readonly sha1: (data: CryptoBytes) => ArrayBuffer;
        /** `n` random bytes (1..65536) from the system CSPRNG. */
        readonly random: (n: number) => ArrayBuffer;
        /** `n` random bytes (1-1024) as unpadded base64url (default) or lowercase hex. */
        readonly randomToken: (n: number, format?: "base64url" | "hex") => string;
        /** PBKDF2-HMAC-SHA256 password hash, as `pbkdf2:iterations:salt_hex:hash_hex`. */
        readonly hashPassword: (password: string) => string;
        /** Whether `password` matches a `hashPassword` string (false for a malformed one). */
        readonly verifyPassword: (password: string, hash: string) => boolean;
        /** A fresh Ed25519 keypair (32-byte public, 64-byte secret key). */
        readonly ed25519Keypair: () => Keypair;
        /** Ed25519 signature (64 bytes) of `data` with a 64-byte secret key. */
        readonly ed25519Sign: (data: CryptoBytes, secretKey: CryptoBytes) => ArrayBuffer;
        /** Whether `signature` is a valid Ed25519 signature of `data` under the 32-byte public key. */
        readonly ed25519Verify: (data: CryptoBytes, signature: CryptoBytes, publicKey: CryptoBytes) => boolean;
        /** Verify an RSA / ECDSA signature against a PEM public key; false when invalid, throws on an unknown alg. */
        readonly verify: (alg: AsymAlg, pubkeyPem: string, data: CryptoBytes, signature: CryptoBytes) => boolean;
        /** Sign with an unencrypted PEM private key; throws when the key cannot sign under `alg`. */
        readonly sign: (alg: AsymAlg, privatePem: string, data: CryptoBytes) => ArrayBuffer;
        /** An RSA private key PEM from big-endian components (CRT values derived, key checked). */
        readonly rsaPrivatePem: (n: CryptoBytes, e: CryptoBytes, d: CryptoBytes, p: CryptoBytes, q: CryptoBytes) => string;
        /** The SubjectPublicKeyInfo PEM of a DER X.509 certificate, or null when it does not parse. */
        readonly x509PubkeyPem: (der: CryptoBytes) => string | null;
        /** HMAC-SHA512/256 tag (32 bytes) of `msg` under a 32-byte key. */
        readonly auth: (msg: CryptoBytes, key: CryptoBytes) => ArrayBuffer;
        /** Whether `tag` is the `auth` tag of `msg` under `key`, in constant time. */
        readonly authVerify: (tag: CryptoBytes, msg: CryptoBytes, key: CryptoBytes) => boolean;
        /** XSalsa20-Poly1305: `msg` sealed with a 24-byte nonce and 32-byte key (ciphertext + 16-byte tag). */
        readonly secretbox: (msg: CryptoBytes, nonce: CryptoBytes, key: CryptoBytes) => ArrayBuffer;
        /** Open a secretbox ciphertext; null when it does not authenticate. */
        readonly secretboxOpen: (ciphertext: CryptoBytes, nonce: CryptoBytes, key: CryptoBytes) => ArrayBuffer | null;
        /** Curve25519-XSalsa20-Poly1305 public-key encryption to `publicKey` from `secretKey`. */
        readonly box: (msg: CryptoBytes, nonce: CryptoBytes, publicKey: CryptoBytes, secretKey: CryptoBytes) => ArrayBuffer;
        /** Open a box ciphertext; null when it does not authenticate. */
        readonly boxOpen: (ciphertext: CryptoBytes, nonce: CryptoBytes, publicKey: CryptoBytes, secretKey: CryptoBytes) => ArrayBuffer | null;
        /** A fresh box keypair (32-byte keys). */
        readonly boxKeypair: () => Keypair;
        /** X25519 shared secret (32 bytes), or null for a low-order peer point. */
        readonly x25519: (secretKey: CryptoBytes, publicKey: CryptoBytes) => ArrayBuffer | null;
        /** A fresh X25519 keypair (32-byte keys). */
        readonly x25519Keypair: () => Keypair;
        /** HMAC-SHA256 (32 bytes) of `data` under a non-empty key. */
        readonly hmacSha256: (data: CryptoBytes, key: CryptoBytes) => ArrayBuffer;
        /** HMAC-SHA1 (20 bytes). HOTP / TOTP only. */
        readonly hmacSha1: (data: CryptoBytes, key: CryptoBytes) => ArrayBuffer;
        /** Whether `expected` is the 32-byte HMAC-SHA256 of `data` under `key`, in constant time. */
        readonly hmacSha256Verify: (data: CryptoBytes, key: CryptoBytes, expected: CryptoBytes) => boolean;
        /** Constant-time equality of two values of the same length (length is not secret). */
        readonly constantTimeEq: (a: CryptoBytes, b: CryptoBytes) => boolean;
        /** OpenSSH bcrypt_pbkdf: `length` bytes from a passphrase and salt; rounds 1..BCRYPT_MAX_ROUNDS. */
        readonly bcryptPbkdf: (passphrase: CryptoBytes, salt: CryptoBytes, rounds: number, length: number) => ArrayBuffer;
        /** bcryptPbkdf over the value of an environment variable in `manifest.env`, never a JS value. */
        readonly bcryptPbkdfEnv: (name: string, salt: CryptoBytes, rounds: number, length: number) => ArrayBuffer;
        /** AES-256-CTR with a 32-byte key and 16-byte IV (both directions; unauthenticated). */
        readonly aes256ctr: (key: CryptoBytes, iv: CryptoBytes, data?: CryptoBytes | null) => ArrayBuffer;
        /** ChaCha20 (RFC 8439: 32-byte key, 12-byte nonce, 32-bit block counter). */
        readonly chacha20: (key: CryptoBytes, nonce: CryptoBytes, counter: number, data?: CryptoBytes | null) => ArrayBuffer;
        /** Poly1305 tag (16 bytes) of `msg` under a ONE-TIME 32-byte key. */
        readonly poly1305: (key: CryptoBytes, msg?: CryptoBytes | null) => ArrayBuffer;
        /** AES-256-GCM seal (32-byte key, 12-byte IV). */
        readonly gcmSeal: (key: CryptoBytes, iv: CryptoBytes, aad?: CryptoBytes | null, plaintext?: CryptoBytes | null) => GcmSealed;
        /** AES-256-GCM open; null for a tag that does not verify. */
        readonly gcmOpen: (key: CryptoBytes, iv: CryptoBytes, aad: CryptoBytes | null | undefined, ciphertext: CryptoBytes | null | undefined, tag: CryptoBytes) => ArrayBuffer | null;
        /** The largest bcrypt_pbkdf round count accepted. */
        readonly BCRYPT_MAX_ROUNDS: number;
    };
}
