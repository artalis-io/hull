/*
 * hull:kv:_seal - the encryption layer of an encrypted kv handle (JS mirror
 * of hull.kv._seal). Built by kv.open({ encrypt }) and consulted by the
 * handle's value methods; plain handles and every hull:cache handle have none.
 * Values are sealed with hull:crypto:sealbox, bound to namespace and key name.
 * Design: docs/kv_encryption_design.md.
 *
 * Internal module. SPDX-License-Identifier: AGPL-3.0-or-later
 */

import util from "hull:kv:_util";
import { sealbox } from "hull:crypto:sealbox";
import { crypto } from "hull:crypto";

function create(encrypt, namespace) {
    if (!encrypt || typeof encrypt !== "object") {
        util.error("invalid_argument",
            "kv.open: encrypt must be { keys: {[id]: key}, current: id }");
    }
    let ring;
    try {
        ring = sealbox.keyring(encrypt);
    } catch (e) {
        util.error("invalid_argument", "kv.open: " + (e && e.message ? e.message : String(e)));
    }
    const allowPlaintext = encrypt.allowPlaintext === true || encrypt.allow_plaintext === true;
    return {
        seal(k, value) { return sealbox.seal(ring, value, [namespace, k]); },

        // [value, version]; version is null for a plaintext value read under
        // allowPlaintext. Throws decrypt_failed.
        open(k, stored) {
            const [value, err, version] = sealbox.open(ring, stored, [namespace, k]);
            if (!err) return [value, version];
            // Migration only: while allowPlaintext is set, a plaintext value
            // planted by a writer is accepted too - the docs say so.
            if (allowPlaintext) return [stored, null];
            util.error("decrypt_failed", "kv: the value for this key does not open with "
                + "the handle's keys (altered, from another key or namespace, "
                + "or sealed with a key not in the keyring)");
        },

        isCurrent(version) { return version === ring.current; },

        // Whether two opened values are equal, in constant time: `cas`
        // compares a secret value with the caller's guess.
        same(a, b) {
            return typeof a === "string" && typeof b === "string" && crypto.constantTimeEq(a, b);
        },
    };
}

export default { create };
