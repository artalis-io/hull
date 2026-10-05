/**
 * @file hull:i18n
 * @module hull:i18n
 * @description Lightweight internationalization. Lua parity: `hull.i18n`.
 *
 * Locale-aware string lookup with `${var}` interpolation, number / date /
 * currency formatting per locale rules, and an `Accept-Language` parser.
 *
 * @license AGPL-3.0-or-later
 *
 * @example
 * import { i18n } from "hull:i18n";
 * i18n.load("en", { hello: "Hello, ${name}!" });
 * i18n.locale(i18n.detect(req.headers["accept-language"]) || "en");
 * res.html(i18n.t("hello", { name: "Alice" }));
 */

// ── Internal state ──────────────────────────────────────────────────

// No prototype: detect() looks request-supplied names up here, and on a
// plain {} "constructor" or "toString" (from Accept-Language) was found.
let locales = Object.create(null);      // name -> locale table
let active = null;     // current locale name

// ── Helpers ─────────────────────────────────────────────────────────

// Traverse a nested object by dotted key path.
function deepGet(obj, key) {
    const parts = key.split(".");
    let node = obj;
    for (let i = 0; i < parts.length; i++) {
        if (node === null || node === undefined || typeof node !== "object")
            return undefined;
        node = node[parts[i]];
    }
    return node;
}

// Replace ${key} placeholders with values from params object.
function interpolate(str, params) {
    if (!params) return str;
    return str.replace(/\$\{(\w+)\}/g, function(match, key) {
        // Own-property lookup only: `${constructor}` / `${toString}` /
        // `${__proto__}` must echo the literal placeholder, not resolve to an
        // Object.prototype member. This matches the Lua sibling (a plain-table
        // `params[k]` has no prototype chain, so a missing key returns nil ->
        // the literal `${key}`).
        if (!Object.prototype.hasOwnProperty.call(params, key)) return match;
        const v = params[key];
        if (v === undefined || v === null) return match;
        return String(v);
    });
}

// Format an integer string with thousands separator.
function formatInt(s, sep) {
    const len = s.length;
    if (len <= 3) return s;
    const parts = [];
    let pos = len % 3;
    if (pos > 0) parts.push(s.substring(0, pos));
    for (let i = pos; i < len; i += 3)
        parts.push(s.substring(i, i + 3));
    return parts.join(sep);
}

// Pure-arithmetic epoch (seconds) to UTC date components.
function epochToUtc(ts) {
    ts = Math.floor(ts);
    const sec = ts % 60; ts = (ts - sec) / 60;
    const min = ts % 60; ts = (ts - min) / 60;
    const hour = ts % 24; ts = (ts - hour) / 24;
    // ts is now days since 1970-01-01
    const z = ts + 719468;
    const era = Math.floor(z / 146097);
    const doe = z - era * 146097;
    const yoe = Math.floor((doe - Math.floor(doe/1460) + Math.floor(doe/36524) - Math.floor(doe/146096)) / 365);
    let y = yoe + era * 400;
    const doy = doe - (365*yoe + Math.floor(yoe/4) - Math.floor(yoe/100));
    const mp = Math.floor((5*doy + 2) / 153);
    const d = doy - Math.floor((153*mp + 2) / 5) + 1;
    const m = mp + (mp < 10 ? 3 : -9);
    if (m <= 2) y = y + 1;
    return { year: y, month: m, day: d, hour: hour, min: min, sec: sec };
}

// Parse Accept-Language header into sorted array of {lang, q}.
function parseAcceptLanguage(header) {
    if (!header || typeof header !== "string") return [];
    const entries = [];
    const parts = header.split(",");
    for (let i = 0; i < parts.length; i++) {
        const part = parts[i].trim();
        const match = part.match(/^([a-zA-Z0-9-]+)(.*)/);
        if (!match) continue;
        const lang = match[1];
        let q = 1.0;
        const qMatch = match[2].match(/;\s*q\s*=\s*([0-9.]+)/);
        if (qMatch) q = parseFloat(qMatch[1]) || 0;
        entries.push({ lang: lang, q: q });
    }
    entries.sort(function(a, b) { return b.q - a.q; });
    return entries;
}

function pad(n, w) {
    let s = String(n);
    while (s.length < w) s = "0" + s;
    return s;
}

// ── Public API ──────────────────────────────────────────────────────

/**
 * Register a translation table under a locale name.
 * @param {string} name  Locale code (e.g. `"en"`, `"hu"`).
 * @param {Object} tbl   Message keys → strings (with `${var}` placeholders).
 */
function load(name, tbl) {
    if (typeof name !== "string" || tbl === null || typeof tbl !== "object")
        throw new Error("i18n.load: expected (string, object)");
    locales[name] = tbl;
}

/**
 * Get or set the active locale.
 *
 * The active locale is PROCESS-GLOBAL: every request shares it. Set it
 * and translate within one synchronous stretch only - a handler that
 * sets it and then awaits (db.async, http.fetch, a timer) can resume to
 * find another request's locale. Concurrent requests should pass the
 * locale explicitly with `tIn`.
 *
 * @param {string} [name]  When passed, sets the active locale.
 * @returns {string|null}  Current locale name after the call.
 */
function locale(name) {
    if (name !== undefined)
        active = name;
    return active;
}

/**
 * Translate a key with `${var}` interpolation. Dotted paths supported.
 * @param {string} key
 * @param {Object} [params]
 * @returns {string}  Translation, or the key itself when missing.
 */
function t(key, params) {
    return tIn(active, key, params);
}

/**
 * Translate a key in an EXPLICIT locale - stateless, so safe across an
 * await: `i18n.tIn(req.locale, "greeting", { name })`. Same lookup,
 * interpolation and fallback (the key itself) as `t`.
 * @param {string|null} loc  Locale name (null / unknown -> the key).
 * @param {string} key
 * @param {Object} [params]
 * @returns {string}
 */
function tIn(loc, key, params) {
    if (typeof loc !== "string" || !locales[loc]) return key;
    const val = deepGet(locales[loc], key);
    if (typeof val !== "string") return key;
    return interpolate(val, params);
}

// The format object of locale `name` (null/unknown -> none: the defaults).
function formatOf(name) {
    const loc = name && Object.prototype.hasOwnProperty.call(locales, name)
        ? locales[name] : null;
    return loc && loc.format;
}

/**
 * Stateless `number`: format with locale `loc`'s separators, whatever the
 * active locale is (use it in handlers that await, as `tIn`).
 * @param {?string} loc
 * @param {number} n
 * @returns {string}
 */
function numberIn(loc, n) {
    if (typeof n !== "number") return String(n);
    if (!Number.isFinite(n)) return String(n);

    const fmt = formatOf(loc);
    const decSep = (fmt && (fmt.decimalSep || fmt.decimal_sep)) || ".";
    const thousSep = (fmt && (fmt.thousandsSep || fmt.thousands_sep)) || ",";

    const negative = n < 0;
    if (negative) n = -n;

    // Past 1e21 a number prints in exponent form; grouping that gave "1e,+21".
    if (n >= 1e21) return (negative ? "-" : "") + String(n);

    // Ten fixed decimals, trailing zeros trimmed. The fraction used to be
    // printed apart (toPrecision): a tiny one leaked an exponent
    // (2.00000000001 -> "2.000000083e-11") and one that rounded to 1 was
    // dropped instead of carried (1.99999999999 -> "1").
    const fixed = n.toFixed(10);
    const dot = fixed.indexOf(".");
    const fracStr = fixed.substring(dot + 1).replace(/0+$/, "");

    let result = formatInt(fixed.substring(0, dot), thousSep);
    if (fracStr !== "") result += decSep + fracStr;

    if (negative && result !== "0") result = "-" + result;
    return result;
}

/**
 * Format a number using the active locale's separators.
 * @param {number} n
 * @returns {string}
 */
function number(n) { return numberIn(active, n); }

/**
 * Stateless `date`: locale `loc`'s pattern.
 * @param {?string} loc
 * @param {number} timestamp  Seconds since epoch.
 * @returns {string}
 */
function dateIn(loc, timestamp) {
    if (typeof timestamp !== "number" || !Number.isFinite(timestamp))
        return String(timestamp);

    const fmt = formatOf(loc);
    const pattern = (fmt && (fmt.datePattern || fmt.date_pattern)) || "YYYY-MM-DD";

    const dt = epochToUtc(timestamp);
    let result = pattern;
    result = result.replace("YYYY", pad(dt.year, 4));
    result = result.replace("MM", pad(dt.month, 2));
    result = result.replace("DD", pad(dt.day, 2));
    result = result.replace("HH", pad(dt.hour, 2));
    result = result.replace("mm", pad(dt.min, 2));
    result = result.replace("ss", pad(dt.sec, 2));
    return result;
}

/**
 * Format a Unix timestamp using the locale's `datePattern`.
 *
 * Supports `YYYY`/`MM`/`DD`/`HH`/`mm`/`ss` tokens.
 *
 * @param {number} timestamp  Seconds since epoch.
 * @returns {string}
 */
function date(timestamp) { return dateIn(active, timestamp); }

/**
 * Stateless `currency`: locale `loc`'s currency table.
 * @param {?string} loc
 * @param {number} amount
 * @param {string} code   ISO 4217 (e.g. `"USD"`).
 * @returns {string}
 */
function currencyIn(loc, amount, code) {
    if (typeof amount !== "number" || typeof code !== "string")
        return String(amount);
    if (!Number.isFinite(amount)) return String(amount) + " " + code;

    const fmt = formatOf(loc);
    const cur = fmt && fmt.currency
        && Object.prototype.hasOwnProperty.call(fmt.currency, code)
        ? fmt.currency[code] : null;

    if (!cur)
        return numberIn(loc, amount) + " " + code;

    // Same as the Lua sibling: either spelling of the option, the sign taken
    // off first, and the magnitude rounded half away from zero in whole minor
    // units (Math.round rounds -x.5 toward +inf, so the runtimes disagreed).
    const dd = cur.decimalDigits !== undefined ? cur.decimalDigits : cur.decimal_digits;
    const digits = dd !== undefined ? dd : 2;
    const scale = Math.pow(10, digits);

    const decSep = (fmt && (fmt.decimalSep || fmt.decimal_sep)) || ".";
    const thousSep = (fmt && (fmt.thousandsSep || fmt.thousands_sep)) || ",";

    // Past 2^53 minor units the integer split is no longer exact (Lua's %d
    // raised there): fall back to the plain number, as the Lua sibling.
    if (Math.abs(amount) * scale >= 9007199254740992)
        return numberIn(loc, amount) + " " + code;
    let neg = amount < 0;
    const units = Math.floor(Math.abs(amount) * scale + 0.5);
    const intPart = Math.floor(units / scale);
    const fracPart = units - intPart * scale;
    if (units === 0) neg = false;   // no "-0.00"

    let result = formatInt(String(intPart), thousSep);

    if (digits > 0) {
        let fracStr = String(fracPart);
        while (fracStr.length < digits) fracStr = "0" + fracStr;
        result += decSep + fracStr;
    }

    if (neg) result = "-" + result;

    const symbol = cur.symbol || code;
    if (cur.position === "after")
        return result + " " + symbol;
    return symbol + result;
}

/**
 * Format an amount in a given currency per the active locale.
 * @param {number} amount
 * @param {string} code   ISO 4217 (e.g. `"USD"`).
 * @returns {string}
 */
function currency(amount, code) { return currencyIn(active, amount, code); }

/**
 * Pick the best matching locale from `Accept-Language` (RFC 7231 q-pairs).
 *
 * @param {string|Object} headerOrReq  Header value OR a request object
 *   (the function calls `.header("Accept-Language")` on it).
 * @returns {string|null}  Matched locale name.
 */
function detect(headerOrReq) {
    let header = headerOrReq;
    // Duck-type: if it has a .header method, call it
    if (headerOrReq !== null && typeof headerOrReq === "object" &&
        typeof headerOrReq.header === "function") {
        header = headerOrReq.header("Accept-Language");
    }
    if (typeof header !== "string") return null;

    const entries = parseAcceptLanguage(header);
    for (let i = 0; i < entries.length; i++) {
        const lang = entries[i].lang;
        // Exact match
        if (locales[lang]) return lang;
        // Base language match
        const base = lang.split("-")[0];
        if (locales[base]) return base;
    }
    // Second pass: match any locale starting with base
    for (let i = 0; i < entries.length; i++) {
        const base = entries[i].lang.split("-")[0];
        const keys = Object.keys(locales);
        for (let j = 0; j < keys.length; j++) {
            if (keys[j].indexOf(base) === 0) return keys[j];
        }
    }
    return null;
}

function reset() {
    locales = Object.create(null);
    active = null;
}

const i18n = { load, locale, t, tIn, number, numberIn, date, dateIn,
               currency, currencyIn, detect, reset };
export { i18n };
