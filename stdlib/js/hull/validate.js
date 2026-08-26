/**
 * @file hull:validate
 * @module hull:validate
 * @description Schema-based input validation. Lua parity: `hull.validate`.
 * @license AGPL-3.0-or-later
 *
 * @example
 * import { validate } from "hull:validate";
 * const [ok, errors] = validate.check(req.json, {
 *     email: { required: true, email: true },
 *     name:  { required: true, trim: true, min: 1, max: 100 },
 *     age:   { type: "integer", min: 0, max: 150 },
 * });
 * if (!ok) return res.status(422).json({ errors });
 */

// Practical RFC-5322 subset. Kept byte-for-byte semantically identical to the
// Lua sibling's email_ok (stdlib/lua/hull/validate.lua): local part starts
// alphanumeric and allows . _ % + -, a single '@', domain starts alphanumeric
// with at least one '.', TLD >= 2 letters. A secondary pass rejects "..",
// leading ".", ".@", "@." which the regex alone would let through. A prior bare
// /^[^\s@]+@[^\s@]+\.[^\s@]+$/ here was materially weaker than Lua (no length
// cap, no ".." / dot-edge rejection, digit TLDs accepted); guarded by
// tests/e2e_validate_parity.sh.
const EMAIL_RE =
    /^[A-Za-z0-9][A-Za-z0-9._+-]*@[A-Za-z0-9][A-Za-z0-9.-]*\.[A-Za-z][A-Za-z]+$/;

function emailOk(s) {
    if (typeof s !== "string") return false;
    if (s.length > 254) return false;
    if (!EMAIL_RE.test(s)) return false;
    if (s.indexOf("..") !== -1) return false;
    if (/^\./.test(s) || s.indexOf(".@") !== -1 || s.indexOf("@.") !== -1) return false;
    return true;
}

// Pattern-validation input cap: 8192 UTF-8 BYTES. Shared contract with the Lua
// sibling (stdlib/lua/hull/validate.lua), where `#value` is a byte count. Values
// over the cap are rejected BEFORE regex evaluation, and the FULL value is
// tested (never a truncated prefix), so an anchored allowlist rule cannot be
// bypassed by appending a payload past the cap and the accept/reject decision is
// byte-identical across runtimes. Guarded by tests/e2e_validate_parity.sh.
const PATTERN_MAX_INPUT_BYTES = 8192;

// True iff the UTF-8 encoding of `s` exceeds `cap` bytes. Counts with an early
// exit and never materializes an encoded copy (no TextEncoder allocation), so a
// huge input costs at most cap+3 bytes of scanning to be rejected rather than a
// full proportional encode.
function utf8ByteLenExceeds(s, cap) {
    let n = 0;
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length) {
            const c2 = s.charCodeAt(i + 1);
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) { n += 4; i++; }  // surrogate pair -> 4 bytes
            else n += 3;                                        // lone high surrogate -> U+FFFD
        } else n += 3;                                          // BMP >= 0x800, or lone surrogate
        if (n > cap) return true;
    }
    return false;
}

/**
 * Validate a data object against a schema.
 *
 * Schema is an object mapping field names to rule objects. Rules
 * (all optional): `required`, `trim`, `type` (`"string"|"number"|"integer"|"boolean"`),
 * `min` / `max` (string length or numeric bound), `pattern` (regex),
 * `oneof` (array of allowed values), `email` (boolean), `fn`
 * (`value => boolean`), `message` (custom error message).
 *
 * @param {Object} data    Input. Non-object input is treated as `{}`.
 * @param {Object} schema  Rules. Non-object → `[true, null]`.
 * @returns {[boolean, Object<string,string>|null]}  `[ok, errors]` where
 *   errors maps field name → message, or `null` on success.
 */
function check(data, schema) {
    if (typeof data !== "object" || data === null) data = {};
    if (typeof schema !== "object" || schema === null) return [true, null];

    let errors = null;

    const fields = Object.keys(schema);
    for (let i = 0; i < fields.length; i++) {
        const field = fields[i];
        const rules = schema[field];
        let value = data[field];
        let err = null;
        const customMsg = rules.message || null;

        // 1. trim (mutates in-place, not an error)
        if (rules.trim && typeof value === "string") {
            value = value.trim();
            data[field] = value;
        }

        // 2. required
        if (rules.required) {
            if (value === undefined || value === null || value === "") {
                err = customMsg || "is required";
            }
        } else {
            // Optional field: if nil/undefined, skip remaining rules
            if (value === undefined || value === null) {
                continue;
            }
        }

        if (err) { setError(); continue; }

        // 3. type check
        if (rules.type) {
            const rt = rules.type;
            if (rt === "string") {
                if (typeof value !== "string")
                    err = customMsg || "must be a string";
            } else if (rt === "number") {
                if (typeof value !== "number")
                    err = customMsg || "must be a number";
            } else if (rt === "integer") {
                if (typeof value !== "number" || !Number.isInteger(value))
                    err = customMsg || "must be an integer";
            } else if (rt === "boolean") {
                if (typeof value !== "boolean")
                    err = customMsg || "must be a boolean";
            }
        }

        if (err) { setError(); continue; }

        // 4. min
        if (rules.min !== undefined) {
            if (typeof value === "string") {
                if (value.length < rules.min)
                    err = customMsg || "must be at least " + rules.min + " characters";
            } else if (typeof value === "number") {
                if (value < rules.min)
                    err = customMsg || "must be at least " + rules.min;
            } else {
                // min/max only bound string length or numeric value; a boolean
                // or object would silently pass otherwise. Fail closed.
                err = customMsg || "must be a string or number";
            }
        }

        if (err) { setError(); continue; }

        // 5. max
        if (rules.max !== undefined) {
            if (typeof value === "string") {
                if (value.length > rules.max)
                    err = customMsg || "must be at most " + rules.max + " characters";
            } else if (typeof value === "number") {
                if (value > rules.max)
                    err = customMsg || "must be at most " + rules.max;
            } else {
                err = customMsg || "must be a string or number";
            }
        }

        if (err) { setError(); continue; }

        // 6. pattern. Bound the pattern (<= 1024 chars) and the value
        // (<= PATTERN_MAX_INPUT_BYTES UTF-8 bytes) so a ReDoS attack via a
        // crafted pattern + input requires a very persistent attacker. We don't
        // accept pre-compiled RegExp objects: their source could contain
        // known-bad backtracking patterns the string check would catch.
        if (rules.pattern !== undefined && rules.pattern !== null) {
            if (rules.pattern instanceof RegExp)
                throw new Error("validate: pass pattern as a string, not a RegExp");
            if (typeof rules.pattern !== "string")
                throw new Error("validate: pattern must be a string");
            if (rules.pattern.length > 1024)
                throw new Error("validate: pattern too long (max 1024 chars)");
            let re;
            try { re = new RegExp(rules.pattern); }
            catch (e) { throw new Error("validate: invalid pattern: " + e.message); }
            // Reject an over-cap value BEFORE regex evaluation and test the FULL
            // value (never a truncated prefix), so an anchored rule can't be
            // bypassed with a payload appended past the cap.
            if (typeof value !== "string"
                || utf8ByteLenExceeds(value, PATTERN_MAX_INPUT_BYTES)
                || !re.test(value))
                err = customMsg || "does not match the required pattern";
        }

        if (err) { setError(); continue; }

        // 7. oneof
        if (rules.oneof) {
            let found = false;
            for (let j = 0; j < rules.oneof.length; j++) {
                if (value === rules.oneof[j]) { found = true; break; }
            }
            if (!found)
                err = customMsg || "must be one of: " + rules.oneof.join(", ");
        }

        if (err) { setError(); continue; }

        // 8. email
        if (rules.email) {
            if (!emailOk(value))
                err = customMsg || "is not a valid email";
        }

        if (err) { setError(); continue; }

        // 9. fn (custom validator)
        if (rules.fn) {
            const fnErr = rules.fn(value, field, data);
            if (fnErr)
                err = fnErr;
        }

        if (err) { setError(); continue; }
        continue;

        function setError() {
            if (!errors) errors = {};
            errors[field] = err;
        }
    }

    if (errors)
        return [false, errors];
    return [true, null];
}

const validate = { check };
export { validate };
