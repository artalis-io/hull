/*
 * hull:validate -- Schema-based data validation
 *
 * validate.check(data, schema) -> [ok, errors]
 *
 * Pure function for validating objects against a schema definition.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

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
            }
        }

        if (err) { setError(); continue; }

        // 6. pattern
        if (rules.pattern) {
            const re = (rules.pattern instanceof RegExp) ? rules.pattern : new RegExp(rules.pattern);
            if (typeof value !== "string" || !re.test(value))
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
