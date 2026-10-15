// hull:_test_hooks - the stdlib modules' test hooks (internal: the underscore
// keeps it out of app code).
//
// A module's test-only helpers reach its private state (decrypt a stored TOTP
// secret, un-mark a processed inbox message, reset its config) and run in a
// stdlib frame, so the _hull_* table check admits their SQL. Exported on the
// module itself (`totp._test`), any app could call them (audit 16). A module
// registers them here instead, under its own import name. Only another hull:
// module can import this one; the C unit-test harness reads it through a
// module of its own under a hull: name. An app, under `hull test` too, has no
// way in.

const hooks = new Map();

function register(name, t) {
    hooks.set(name, t);
}

function get(name) {
    return hooks.get(name);
}

export const _test_hooks = { register, get };
export default _test_hooks;
