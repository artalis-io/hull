/**
 * @file hull:config
 * @module hull:config
 * @description Typed application configuration over the environment. Lua parity:
 *   `hull.config`.
 *
 * A thin, fail-fast layer over the `env` capability: read a setting with a type,
 * a default, and optional required-ness instead of hand-rolling
 * `Number(env.get("PORT")) || 8080` at every call site. The env allowlist still
 * applies - a name must be in `manifest.env` for `env.get` to return it (an
 * un-allowlisted name reads as absent -> default, or throws if `required`).
 *
 * Errors follow the stdlib convention (docs/stdlib_style.md section 1): a
 * missing required value or an uncoercible value THROWS; a present-and-valid or
 * absent-with-default value is returned.
 *
 * @license AGPL-3.0-or-later
 * @example
 *   import { config } from "hull:config";
 *   const port  = config.get("PORT", { type: "integer", default: 8080 });
 *   const dsn   = config.require("DATABASE_URL");            // throws if unset
 *   const debug = config.get("DEBUG", { type: "boolean", default: false });
 */

import { env } from "hull:env";
import { fs } from "hull:fs";

const config = {};

// Values loaded from a .env file by config.loadDotenv. ONLY names in the
// manifest env allowlist are ever stored (see loadDotenv), so this fallback
// can never widen what config exposes beyond manifest.env.
const _dotenv = {};

const TRUTHY = { "1": true, "true": true, "yes": true, "on": true };
const FALSY = { "0": true, "false": true, "no": true, "off": true, "": true };

function coerce(name, raw, ty) {
    if (ty === undefined || ty === "string") return raw;
    if (ty === "boolean") {
        const k = raw.toLowerCase();
        if (TRUTHY[k]) return true;
        if (FALSY[k]) return false;
        throw new Error("config: " + name + " is not a boolean: '" + raw + "'");
    }
    if (ty === "number") {
        const n = Number(raw);
        if (!Number.isFinite(n)) throw new Error("config: " + name + " is not a number: '" + raw + "'");
        return n;
    }
    if (ty === "integer") {
        const n = Number(raw);
        if (!Number.isFinite(n) || !Number.isInteger(n))
            throw new Error("config: " + name + " is not an integer: '" + raw + "'");
        return n;
    }
    throw new Error("config: unknown type '" + String(ty) + "' for " + name);
}

/**
 * Read a configuration value.
 * @param {string} name  Environment variable name (must be in manifest.env).
 * @param {Object} [opts]
 * @param {("string"|"number"|"integer"|"boolean")} [opts.type="string"]
 * @param {*} [opts.default]  Returned when absent. NOT type-coerced.
 * @param {boolean} [opts.required=false]  Throw if the value is absent.
 * @returns {*} The typed value, or `default` when absent.
 */
config.get = function(name, opts) {
    opts = opts || {};
    // Precedence: the real process env (allowlisted env cap) wins; a .env value
    // (also allowlist-gated at load time) backs a declared-but-unset name; then
    // the default.
    let raw = env.get(name);
    if ((raw === null || raw === undefined || raw === "") &&
        Object.prototype.hasOwnProperty.call(_dotenv, name)) {
        raw = _dotenv[name];
    }
    if (raw === null || raw === undefined || raw === "") {
        if (opts.required)
            throw new Error("config: required setting " + String(name) + " is not set");
        return opts.default;
    }
    return coerce(name, raw, opts.type);
};

function stripQuotes(v) {
    if (v.length >= 2) {
        const q = v[0];
        if ((q === '"' || q === "'") && v[v.length - 1] === q) return v.slice(1, -1);
    }
    return v;
}

/**
 * Load a `.env` file so its values back declared settings. Two gates, both
 * enforced: (1) the file is read through the `fs` capability, so the path must
 * be in `manifest.fs.read` (a denied/missing file is a silent no-op -> 0); (2) a
 * `KEY=value` line is applied ONLY when `KEY` is in the manifest env allowlist
 * (`env.allowed(KEY)`), so `.env` can never introduce a value for an
 * undeclared name. The real process env always overrides a `.env` value. Call
 * once at startup before the first config.get.
 *
 * @param {string} [path=".env"]  File path (must be in manifest.fs.read).
 * @returns {number} Number of allowlisted keys applied.
 */
config.loadDotenv = function(path) {
    path = path || ".env";
    let ab;
    try { ab = fs.read(path); } catch (e) { return 0; }   // denied / missing
    const bytes = new Uint8Array(ab);
    let content = "";
    for (let i = 0; i < bytes.length; i++) content += String.fromCharCode(bytes[i]);
    let n = 0;
    const lines = content.split("\n");
    for (let i = 0; i < lines.length; i++) {
        let s = lines[i].trim();
        if (s === "" || s[0] === "#") continue;
        s = s.replace(/^export\s+/, "");
        const eq = s.indexOf("=");
        if (eq < 0) continue;
        const k = s.slice(0, eq).replace(/\s+$/, "");
        const v = stripQuotes(s.slice(eq + 1).replace(/^\s+/, ""));
        if (k !== "" && env.allowed(k)) { _dotenv[k] = v; n++; }
    }
    return n;
};

/**
 * Read a required configuration value (throws if absent).
 * @param {string} name
 * @param {Object} [opts]  Accepts `type` (see config.get).
 * @returns {*} The typed value.
 */
config.require = function(name, opts) {
    opts = opts || {};
    return config.get(name, { type: opts.type, required: true });
};

export { config };
export default config;
