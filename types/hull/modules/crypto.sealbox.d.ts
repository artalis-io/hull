// hull:crypto:sealbox - versioned, authenticated sealing of values at rest.
declare module "hull:crypto:sealbox" {
    /** A key id: an integer 0..2^32-1, or the same in canonical decimal text. */
    export type SealboxKeyId = number | string;

    /** The options of `sealbox.keyring`. */
    export interface KeyringOptions {
        /** Key id -> a 32-byte byte string or a `crypto.keyFromEnv` key. */
        keys: { [id: string]: string | import("hull:crypto").CryptoKey };
        /** The id new seals use; must be in `keys`. */
        current: SealboxKeyId;
    }

    /** The options of `sealbox.keyringFromEnv`. */
    export interface KeyringFromEnvOptions {
        /** Key id -> the name of an environment variable in `manifest.env`. */
        keys: { [id: string]: string };
        /** The id new seals use; must be in `keys`. */
        current: SealboxKeyId;
    }

    /** A validated keyring. */
    export interface Keyring {
        /** Key id -> key bytes or a C-held key. */
        readonly keys: { readonly [id: number]: Uint8Array | import("hull:crypto").CryptoKey };
        /** The id new seals use. */
        readonly current: number;
    }

    /** Why `sealbox.open` refused a blob. */
    export type SealboxError = "unknown_version" | "open_failed";

    /** A seal context: byte strings bound into the box and checked on open. */
    export type SealboxContext = readonly string[] | null | undefined;

    /** The sealbox functions and constants. */
    export interface Sealbox {
        /** Length of the version prefix, 4. */
        readonly VERSION_LEN: number;
        /** Length of the nonce, 24. */
        readonly NONCE_LEN: number;
        /** Length of the MAC, 16. */
        readonly MAC_LEN: number;
        /** The shortest sealed blob. */
        readonly MIN_LEN: number;
        /** A keyring from `{ keys, current }`; throws on anything malformed. */
        readonly keyring: (opts: KeyringOptions) => Keyring;
        /** A keyring whose keys are read from environment variables into C memory. */
        readonly keyringFromEnv: (opts: KeyringFromEnvOptions) => Keyring;
        /** Seal a byte-string value under the current key, bound to `context`; returns a byte string. */
        readonly seal: (ring: Keyring, value: string, context?: SealboxContext) => string;
        /** `[value, null, version]`, or `[null, reason]` when the blob is not genuine for `context`. */
        readonly open: (ring: Keyring, blob: string, context?: SealboxContext) =>
            [value: string, error: null, version: number] | [value: null, error: SealboxError, version?: undefined];
        /** Open an older unversioned blob (nonce || box) with key `id`; null when it does not open. */
        readonly openUnversioned: (ring: Keyring, id: SealboxKeyId, blob: string) => string | null;
    }

    /** Versioned sealing. */
    export const sealbox: Sealbox;
    export default sealbox;
}
