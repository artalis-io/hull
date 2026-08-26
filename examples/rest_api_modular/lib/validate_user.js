// lib/validate_user.js - Schema validation for the User resource.

import { validate } from "hull:validate";

export function create(body) {
    return validate.check(body, {
        email: { required: true, type: "string", email: true, max: 254 },
        name:  { required: true, type: "string", trim: true, min: 1, max: 100 },
    });
}
