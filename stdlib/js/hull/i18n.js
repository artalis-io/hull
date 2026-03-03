/*
 * hull:i18n -- Lightweight internationalization
 *
 * Locale-aware string lookup with interpolation, number/date/currency
 * formatting, and Accept-Language detection.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// ── Internal state ──────────────────────────────────────────────────

let locales = {};      // name -> locale table
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

function load(name, tbl) {
    if (typeof name !== "string" || tbl === null || typeof tbl !== "object")
        throw new Error("i18n.load: expected (string, object)");
    locales[name] = tbl;
}

function locale(name) {
    if (name !== undefined)
        active = name;
    return active;
}

function t(key, params) {
    if (!active || !locales[active]) return key;
    const val = deepGet(locales[active], key);
    if (typeof val !== "string") return key;
    return interpolate(val, params);
}

function number(n) {
    if (typeof n !== "number") return String(n);

    const loc = active && locales[active];
    const fmt = loc && loc.format;
    const decSep = (fmt && fmt.decimalSep) || ".";
    const thousSep = (fmt && fmt.thousandsSep) || ",";

    const negative = n < 0;
    if (negative) n = -n;

    const intPart = Math.floor(n);
    const fracPart = n - intPart;

    let result = formatInt(String(intPart), thousSep);

    if (fracPart > 0) {
        // Convert to string and strip leading "0."
        let fracStr = String(parseFloat(fracPart.toPrecision(10)));
        const dotPos = fracStr.indexOf(".");
        if (dotPos >= 0)
            result += decSep + fracStr.substring(dotPos + 1);
    }

    if (negative) result = "-" + result;
    return result;
}

function date(timestamp) {
    if (typeof timestamp !== "number") return String(timestamp);

    const loc = active && locales[active];
    const fmt = loc && loc.format;
    const pattern = (fmt && fmt.datePattern) || "YYYY-MM-DD";

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

function currency(amount, code) {
    if (typeof amount !== "number" || typeof code !== "string")
        return String(amount);

    const loc = active && locales[active];
    const fmt = loc && loc.format;
    const cur = fmt && fmt.currency && fmt.currency[code];

    if (!cur)
        return number(amount) + " " + code;

    const digits = (cur.decimalDigits !== undefined) ? cur.decimalDigits : 2;
    const factor = Math.pow(10, digits);
    const rounded = Math.round(amount * factor) / factor;

    const decSep = (fmt && fmt.decimalSep) || ".";
    const thousSep = (fmt && fmt.thousandsSep) || ",";

    let intPart = Math.floor(Math.abs(rounded));
    const fracPart = Math.abs(rounded) - intPart;
    const neg = rounded < 0;

    let result = formatInt(String(intPart), thousSep);

    if (digits > 0) {
        let fracStr = String(Math.round(fracPart * factor));
        while (fracStr.length < digits) fracStr = "0" + fracStr;
        result += decSep + fracStr;
    }

    if (neg) result = "-" + result;

    const symbol = cur.symbol || code;
    if (cur.position === "after")
        return result + " " + symbol;
    return symbol + result;
}

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
    locales = {};
    active = null;
}

const i18n = { load, locale, t, number, date, currency, detect, reset };
export { i18n };
