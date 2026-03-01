/*
 * hull:template -- Server-side template engine
 *
 * template.render(name, data)           - load + compile + render (cached)
 * template.renderString(source, data)   - compile from string + render
 * template.compile(name)                - returns compiled function
 * template.clearCache()                 - clear compiled function cache
 *
 * Syntax:
 *   {{ var }}              HTML-escaped output
 *   {{ var.path }}         dot path lookup (nil-safe)
 *   {{ var | filter }}     pipe filter
 *   {{ var | filter: arg }}filter with argument
 *   {{{ var }}}            raw (unescaped) output
 *   {% if var %}           conditional
 *   {% if not var %}       negated conditional
 *   {% elif var %}         else-if
 *   {% elif not var %}     negated else-if
 *   {% else %}             else
 *   {% end %}              end block
 *   {% for item in list %} iterate array
 *   {% for key, val in obj %} iterate key/value pairs
 *   {% block name %}       define overridable block
 *   {% extends "name" %}   inherit from parent template
 *   {% include "name" %}   include partial
 *   {# comment #}         stripped from output
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { _template } from "hull:_template";

// ── HTML escape ──────────────────────────────────────────────────────

const escapeMap = {
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;",
};

function htmlEscape(s) {
    if (s == null) return "";
    s = String(s);
    return s.replace(/[&<>"']/g, (ch) => escapeMap[ch]);
}

// ── Built-in filters ────────────────────────────────────────────────

const filters = {
    upper(val) { return String(val ?? "").toUpperCase(); },
    lower(val) { return String(val ?? "").toLowerCase(); },
    trim(val) { return String(val ?? "").trim(); },
    length(val) {
        if (Array.isArray(val)) return val.length;
        if (val && typeof val === "object") return Object.keys(val).length;
        return String(val ?? "").length;
    },
    default(val, fallback) {
        if (val == null || val === false || val === "") return fallback ?? "";
        return val;
    },
    json(val) { return JSON.stringify(val); },
    raw(val) { return val; },
};

// ── Lexer ────────────────────────────────────────────────────────────

const T_TEXT = "text";
const T_VAR = "var";
const T_RAW = "raw";
const T_TAG = "tag";

function lex(source) {
    const tokens = [];
    let pos = 0;
    const len = source.length;

    while (pos < len) {
        const dStart = source.indexOf("{", pos);
        if (dStart === -1) {
            if (pos < len) tokens.push({ type: T_TEXT, value: source.slice(pos) });
            break;
        }

        if (dStart > pos) {
            tokens.push({ type: T_TEXT, value: source.slice(pos, dStart) });
        }

        const c3 = source.slice(dStart, dStart + 3);
        const c2 = source.slice(dStart, dStart + 2);

        if (c3 === "{{{") {
            const close = source.indexOf("}}}", dStart + 3);
            if (close === -1) throw new Error("unclosed {{{ at position " + dStart);
            tokens.push({ type: T_RAW, value: source.slice(dStart + 3, close).trim() });
            pos = close + 3;
        } else if (c2 === "{{") {
            const close = source.indexOf("}}", dStart + 2);
            if (close === -1) throw new Error("unclosed {{ at position " + dStart);
            tokens.push({ type: T_VAR, value: source.slice(dStart + 2, close).trim() });
            pos = close + 2;
        } else if (c2 === "{%") {
            const close = source.indexOf("%}", dStart + 2);
            if (close === -1) throw new Error("unclosed {% at position " + dStart);
            tokens.push({ type: T_TAG, value: source.slice(dStart + 2, close).trim() });
            pos = close + 2;
        } else if (c2 === "{#") {
            const close = source.indexOf("#}", dStart + 2);
            if (close === -1) throw new Error("unclosed {# at position " + dStart);
            pos = close + 2;
        } else {
            tokens.push({ type: T_TEXT, value: "{" });
            pos = dStart + 1;
        }
    }

    return tokens;
}

// ── Parser ───────────────────────────────────────────────────────────

function parseExpr(expr) {
    const parts = expr.split("|").map(s => s.trim());
    const varPart = parts[0];
    const filterChain = [];

    for (let i = 1; i < parts.length; i++) {
        const f = parts[i];
        let m = f.match(/^(\w+)\s*:\s*(.+)$/);
        if (m) {
            filterChain.push({ name: m[1], arg: m[2].trim() });
        } else {
            m = f.match(/^(\w+)$/);
            if (m) filterChain.push({ name: m[1], arg: null });
        }
    }

    return { var: varPart, filters: filterChain };
}

function parseTag(tag) {
    let m;

    m = tag.match(/^extends\s+["']([^"']+)["']$/);
    if (m) return { kind: "extends", name: m[1] };

    m = tag.match(/^include\s+["']([^"']+)["']$/);
    if (m) return { kind: "include", name: m[1] };

    m = tag.match(/^block\s+(\S+)$/);
    if (m) return { kind: "block", name: m[1] };

    if (tag === "end") return { kind: "end" };
    if (tag === "else") return { kind: "else" };

    m = tag.match(/^elif\s+(not)\s+(.+)$/);
    if (m) return { kind: "elif", cond: m[2], negated: true };
    m = tag.match(/^elif\s+(.+)$/);
    if (m) return { kind: "elif", cond: m[1], negated: false };

    m = tag.match(/^if\s+(not)\s+(.+)$/);
    if (m) return { kind: "if", cond: m[2], negated: true };
    m = tag.match(/^if\s+(.+)$/);
    if (m) return { kind: "if", cond: m[1], negated: false };

    m = tag.match(/^for\s+(\w+)\s*,\s*(\w+)\s+in\s+(.+)$/);
    if (m) return { kind: "for_kv", key: m[1], val: m[2], expr: m[3] };

    m = tag.match(/^for\s+(\w+)\s+in\s+(.+)$/);
    if (m) return { kind: "for", var: m[1], expr: m[2] };

    throw new Error("unknown tag: {% " + tag + " %}");
}

function parse(tokens) {
    let pos = 0;

    function parseBody(stopKinds) {
        const body = [];
        while (pos < tokens.length) {
            const tok = tokens[pos];

            if (tok.type === T_TEXT) {
                body.push({ kind: "text", value: tok.value });
                pos++;
            } else if (tok.type === T_VAR) {
                body.push({ kind: "var", expr: parseExpr(tok.value) });
                pos++;
            } else if (tok.type === T_RAW) {
                body.push({ kind: "raw", expr: parseExpr(tok.value) });
                pos++;
            } else if (tok.type === T_TAG) {
                const tag = parseTag(tok.value);
                pos++;

                if (tag.kind === "end" || tag.kind === "else" || tag.kind === "elif") {
                    if (stopKinds) {
                        for (let i = 0; i < stopKinds.length; i++) {
                            if (stopKinds[i] === tag.kind) return { body, stop: tag };
                        }
                    }
                    throw new Error("unexpected {% " + tag.kind + " %}");
                }

                if (tag.kind === "extends") {
                    body.push({ kind: "extends", name: tag.name });
                } else if (tag.kind === "include") {
                    body.push({ kind: "include", name: tag.name });
                } else if (tag.kind === "block") {
                    const r = parseBody(["end"]);
                    body.push({ kind: "block", name: tag.name, body: r.body });
                } else if (tag.kind === "if") {
                    const branches = [];
                    let r = parseBody(["end", "else", "elif"]);
                    branches.push({ cond: tag.cond, negated: tag.negated, body: r.body });

                    let stop = r.stop;
                    while (stop && stop.kind === "elif") {
                        const elifCond = stop.cond;
                        const elifNeg = stop.negated;
                        r = parseBody(["end", "else", "elif"]);
                        branches.push({ cond: elifCond, negated: elifNeg, body: r.body });
                        stop = r.stop;
                    }

                    let elseBody = null;
                    if (stop && stop.kind === "else") {
                        r = parseBody(["end"]);
                        elseBody = r.body;
                    }

                    body.push({ kind: "if", branches, elseBody });
                } else if (tag.kind === "for") {
                    const r = parseBody(["end"]);
                    body.push({ kind: "for", var: tag.var, expr: tag.expr, body: r.body });
                } else if (tag.kind === "for_kv") {
                    const r = parseBody(["end"]);
                    body.push({ kind: "for_kv", key: tag.key, val: tag.val, expr: tag.expr, body: r.body });
                }
            }
        }
        return { body, stop: null };
    }

    return parseBody(null).body;
}

// ── Inheritance + include resolution ────────────────────────────────

function collectBlocks(ast) {
    const blocks = {};
    for (const node of ast) {
        if (node.kind === "block") blocks[node.name] = node.body;
    }
    return blocks;
}

function findExtends(ast) {
    for (const node of ast) {
        if (node.kind === "extends") return node.name;
    }
    return null;
}

function applyBlocks(ast, overrides) {
    const result = [];
    for (const node of ast) {
        if (node.kind === "block") {
            const replacement = overrides[node.name] || node.body;
            for (const child of replacement) result.push(child);
        } else {
            result.push(node);
        }
    }
    return result;
}

function resolveInheritance(ast, loadFn, visited) {
    visited = visited || {};
    const parentName = findExtends(ast);
    if (!parentName) return ast;

    if (visited[parentName]) throw new Error("circular extends: " + parentName);
    visited[parentName] = true;

    const parentSource = loadFn(parentName);
    if (parentSource == null) throw new Error("template not found: " + parentName);

    let parentAst = parse(lex(parentSource));
    parentAst = resolveInheritance(parentAst, loadFn, visited);

    const childBlocks = collectBlocks(ast);
    return applyBlocks(parentAst, childBlocks);
}

function resolveIncludes(ast, loadFn, visited) {
    visited = visited || {};
    const result = [];
    for (const node of ast) {
        if (node.kind === "include") {
            if (visited[node.name]) throw new Error("circular include: " + node.name);
            visited[node.name] = true;
            const source = loadFn(node.name);
            if (source == null) throw new Error("template not found: " + node.name);
            let incAst = parse(lex(source));
            incAst = resolveIncludes(incAst, loadFn, visited);
            for (const n of incAst) result.push(n);
        } else if (node.kind === "if") {
            const newBranches = node.branches.map(b => ({
                cond: b.cond, negated: b.negated,
                body: resolveIncludes(b.body, loadFn, visited)
            }));
            const newElse = node.elseBody ? resolveIncludes(node.elseBody, loadFn, visited) : null;
            result.push({ kind: "if", branches: newBranches, elseBody: newElse });
        } else if (node.kind === "for") {
            result.push({ kind: "for", var: node.var, expr: node.expr,
                          body: resolveIncludes(node.body, loadFn, visited) });
        } else if (node.kind === "for_kv") {
            result.push({ kind: "for_kv", key: node.key, val: node.val, expr: node.expr,
                          body: resolveIncludes(node.body, loadFn, visited) });
        } else if (node.kind === "block") {
            result.push({ kind: "block", name: node.name,
                          body: resolveIncludes(node.body, loadFn, visited) });
        } else {
            result.push(node);
        }
    }
    return result;
}

// ── JS code generator ───────────────────────────────────────────────

// Generate optional-chaining dot path access
// "user.name" → "__d.user?.name"
// "title" → "__d.title"
// If the first segment is a loop-local variable, use it directly.
function genDotPath(path, prefix, localsSet) {
    prefix = prefix || "__d";
    const parts = path.split(".");

    // Check if first segment is a loop-local variable
    if (localsSet && localsSet[parts[0]]) {
        if (parts.length === 1) return parts[0];
        return parts[0] + parts.slice(1).map(p => "?." + p).join("");
    }

    if (parts.length === 1) return prefix + "." + parts[0];
    return prefix + "." + parts[0] + parts.slice(1).map(p => "?." + p).join("");
}

function genExpr(exprInfo, escaped, localsSet) {
    let code = genDotPath(exprInfo.var, null, localsSet);

    for (const f of exprInfo.filters) {
        if (f.name === "raw") {
            escaped = false;
        } else if (f.arg) {
            let arg = f.arg.trim();
            if (arg[0] === '"' || arg[0] === "'") {
                code = "__f." + f.name + "(" + code + ", " + arg + ")";
            } else {
                code = "__f." + f.name + "(" + code + ", " + genDotPath(arg, null, localsSet) + ")";
            }
        } else {
            code = "__f." + f.name + "(" + code + ")";
        }
    }

    if (escaped) code = "__e(" + code + ")";
    return code;
}

function genCond(cond, negated, localsSet) {
    const path = genDotPath(cond.trim(), null, localsSet);
    return negated ? "!" + path : path;
}

function jsQuote(s) {
    return JSON.stringify(s);
}

function codegen(ast) {
    const lines = [];
    let indent = 1;
    const localsSet = {};  // track loop-local variable names

    function emit(line) {
        lines.push("  ".repeat(indent) + line);
    }

    function genBody(nodes) {
        for (const node of nodes) {
            if (node.kind === "text") {
                if (node.value.length > 0) emit("__p.push(" + jsQuote(node.value) + ");");
            } else if (node.kind === "var") {
                emit("__p.push(" + genExpr(node.expr, true, localsSet) + ");");
            } else if (node.kind === "raw") {
                emit("__p.push(String(" + genExpr(node.expr, false, localsSet) + " ?? \"\"));");
            } else if (node.kind === "if") {
                for (let i = 0; i < node.branches.length; i++) {
                    const b = node.branches[i];
                    const keyword = i === 0 ? "if" : "} else if";
                    emit(keyword + " (" + genCond(b.cond, b.negated, localsSet) + ") {");
                    indent++;
                    genBody(b.body);
                    indent--;
                }
                if (node.elseBody) {
                    emit("} else {");
                    indent++;
                    genBody(node.elseBody);
                    indent--;
                }
                emit("}");
            } else if (node.kind === "for") {
                emit("for (const " + node.var + " of (" + genDotPath(node.expr, null, localsSet) + " || [])) {");
                localsSet[node.var] = true;
                indent++;
                genBody(node.body);
                indent--;
                delete localsSet[node.var];
                emit("}");
            } else if (node.kind === "for_kv") {
                emit("for (const [" + node.key + ", " + node.val + "] of Object.entries(" + genDotPath(node.expr, null, localsSet) + " || {})) {");
                localsSet[node.key] = true;
                localsSet[node.val] = true;
                indent++;
                genBody(node.body);
                indent--;
                delete localsSet[node.key];
                delete localsSet[node.val];
                emit("}");
            } else if (node.kind === "block") {
                genBody(node.body);
            }
        }
    }

    lines.push("(function(__d, __e, __f) {");
    lines.push("  const __p = [];");
    genBody(ast);
    lines.push("  return __p.join(\"\");");
    lines.push("})");

    return lines.join("\n");
}

// ── Compile + cache ─────────────────────────────────────────────────

const cache = {};

function loadRaw(name) {
    return _template.loadRaw(name);
}

function compileSource(source, name) {
    const tokens = lex(source);
    let ast = parse(tokens);
    ast = resolveInheritance(ast, loadRaw);
    ast = resolveIncludes(ast, loadRaw);
    const code = codegen(ast);
    const chunkName = name ? "template:" + name : "template";
    return _template.compile(code, chunkName);
}

function compile(name) {
    if (cache[name]) return cache[name];

    const source = loadRaw(name);
    if (source == null) throw new Error("template not found: " + name);

    const fn = compileSource(source, name);
    cache[name] = fn;
    return fn;
}

function render(name, data) {
    const fn = compile(name);
    return fn(data || {}, htmlEscape, filters);
}

function renderString(source, data) {
    const fn = compileSource(source, null);
    return fn(data || {}, htmlEscape, filters);
}

function clearCache() {
    for (const k in cache) delete cache[k];
}

const template = { render, renderString, compile, clearCache };
export { template };
