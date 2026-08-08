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
 *     rl.info("handled");    // -> "handled requestId=... user=..."
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
// sorted for deterministic, cross-runtime-identical output. Value escaping +
// quoting is the shared hull:web:_logfmt rule (escapes \ CR LF ", quotes when
// needed) - the logger middleware uses the same one.
function fmt(fields) {
    if (!fields) return "";
    const keys = Object.keys(fields).sort();
    if (keys.length === 0) return "";
    const parts = [];
    for (let i = 0; i < keys.length; i++) {
        parts.push(_logfmt.pair(keys[i], fields[keys[i]]));
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
        self[lvl] = (msg) => log[lvl](String(msg == null ? "" : msg) + fmt(fields));
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

// Bare pass-throughs (no bound fields), so logx can stand in for log.
for (let i = 0; i < LEVELS.length; i++) logx[LEVELS[i]] = log[LEVELS[i]];

export { logx };
export default logx;
