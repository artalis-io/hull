/**
 * @file hull:logx
 * @module hull:logx
 * @description Contextual logging: bind a set of fields once, log them on every
 *   line. Lua parity: `hull.logx`.
 *
 * A thin, pure layer over the built-in `hull:log`. `logx.with({...})` returns a
 * child logger whose info/warn/error/debug append the bound fields to the
 * message in logfmt form (`key=value`, quoted when needed), so per-request
 * context rides every line without threading it through every call.
 *
 *     import { logx } from "hull:logx";
 *     const rl = logx.with({ requestId: id, user: uid });
 *     rl.info("handled");    // -> 'msg="handled" requestId=... user=...'
 *
 * The message is the line's leading msg="..." field, quoted and escaped as a
 * value (audit 11): written bare, a message carrying " user=admin" forged a
 * field for any logfmt reader. The bare logx.info(msg) (no fields) writes the
 * same msg="..." field, and a bound field named msg goes out as _msg so it
 * cannot override the message (audit 12).
 *     rl.with({ step: 2 }).warn("slow");  // children compose
 *
 * CAVEAT (source tag): hull:log tags each line by the CALLER's source, and the
 * two runtimes resolve that differently for a wrapped call. In JS a wrapper line
 * tags [hull:js] (QuickJS reports the immediate module, hull:logx); in Lua it
 * stays [app]. Message + fields are identical either way - only the JS source
 * tag shifts. When you need a guaranteed [app] tag in BOTH runtimes, use the
 * escape hatch: log.info("handled" + logx.fields({...})) - the app calls log
 * directly, so the tag stays [app]. (A future C-level log.with would make the
 * bound logger tag [app] in JS too.)
 *
 * @license AGPL-3.0-or-later
 */

import { log } from "hull:log";
import { _logfmt } from "hull:web:_logfmt";

const logx = {};

const LEVELS = ["info", "warn", "error", "debug"];

// Format a fields object as a leading-space logfmt fragment " k=v k2=v2". Keys
// sorted for deterministic, cross-runtime-identical output. Key + value
// escaping and quoting is the shared hull:web:_logfmt rule (keys reduced to
// [A-Za-z0-9_.-], values with \ " and control characters escaped, quoted when
// needed) - the logger middleware uses the same one. The message's control
// characters are escaped too (_logfmt.message).
function fmt(fields) {
    if (!fields) return "";
    const keys = Object.keys(fields).sort();
    if (keys.length === 0) return "";
    const parts = [];
    for (let i = 0; i < keys.length; i++) {
        // msg is the message's key (the line's leading msg="..."), so a bound
        // field that reduces to it goes out as _msg (audit 12): a second msg=
        // pair overrode the message for a reader that keeps the last value.
        const name = _logfmt.key(keys[i]) === "msg" ? "_msg" : keys[i];
        parts.push(_logfmt.pair(name, fields[keys[i]]));
    }
    return " " + parts.join(" ");
}

/**
 * Format a fields object into a leading-space logfmt fragment. Escape hatch for
 * keeping the [app] source tag: log.info("msg" + logx.fields({...})).
 * @param {Object} [fields]
 * @returns {string} e.g. " requestId=abc user=42" ("" for no fields).
 */
logx.fields = function(fields) {
    return fmt(fields);
};

function make(fields) {
    const self = {};
    for (let i = 0; i < LEVELS.length; i++) {
        const lvl = LEVELS[i];
        self[lvl] = (msg) => log[lvl](_logfmt.message(msg == null ? "" : msg) + fmt(fields));
    }
    self.with = (more) => make(Object.assign({}, fields, more || {}));
    return self;
}

/**
 * Create a bound logger carrying `fields`.
 * @param {Object} [fields]
 * @returns {Object} A logger with info/warn/error/debug and with().
 */
logx.with = function(fields) {
    return make(fields || {});
};

// Bare levels (no bound fields), so logx can stand in for log. The message goes
// through the same msg="..." quoting as a bound logger's (audit 12): passed
// straight to log, a bare logx.info carried a raw newline or a forged
// " user=admin" field into the line.
for (let i = 0; i < LEVELS.length; i++) {
    const lvl = LEVELS[i];
    logx[lvl] = (msg) => log[lvl](_logfmt.message(msg == null ? "" : msg));
}

export { logx };
export default logx;
