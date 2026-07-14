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

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

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

        // 6. pattern. Bound both the pattern (≤ 1024 chars) and the value
        // (≤ 8192 chars) to make a ReDoS attack via crafted pattern + input
        // require a very persistent attacker. We don't accept pre-compiled
        // RegExp objects: their source could contain known-bad backtracking
        // patterns the string check would catch.
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
            if (typeof value !== "string" || !re.test(String(value).substring(0, 8192)))
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
            if (typeof value !== "string" || !EMAIL_RE.test(value))
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
