// hull:source:frontend_javascript - the JS frontend adapter (mirror of
// hull.project.frontend_lua). Turns the Slice 2-4 pipeline (parser + annotations + scope) into
// the SAME normalized per-source facts hull.project.model consumes, plus declarationSemantics and
// the scope capability, all over a session-retained AST. Runs INSIDE the QuickJS tooling session.
//
//   analyze(bytes, path, opts)   -> { schema_version, status, unit_id, diagnostics, declarations }
//   declarationSemantics(declId) -> a frontend-specific record | { error }
//   scope(unitId)                -> the Slice-4 { ok, bindings, refs } | { ok:false, error }
//
// Bridge-private handles are INTEGERS (unit_id, decl_id); the retained AST / node / path never
// cross the wire. Session module-scope state is dropped on JS_FreeRuntime (a closed generation).
// NEVER raises: every entry is protected; an internal defect becomes a structured result.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { parse } from "hull:source:parser";
import { resolve as scopeResolve } from "hull:source:scope";
import { position } from "hull:source:range";

export const capabilities = ["declarations", "annotations", "source_ranges", "scope", "semantics"];

// ---- session module-scope retained state (per QuickJS runtime = per generation) ----
const units = new Map();     // unit_id -> { ast, comments, linemap, nodes: Set<declarationNode> }
const decls = new Map();     // decl_id -> { unit_id, formKind, kind, node, declarator_index, binding_path, name }
let nextUnitId = 0;
let nextDeclId = 0;

// ---- range normalization (Lua mkrange parity: { start, stop, line, col }) ----
function nrange(linemap, start, stop) {
    const p = position(linemap, start);
    return { start: start, stop: stop, line: p.line, col: p.col };
}
function nodeRange(linemap, node) { return nrange(linemap, node.start, node.stop); }
function annRange(linemap, r) { return (r && typeof r.start === "number") ? nrange(linemap, r.start, r.stop) : null; }

// Slice-3 annotation records { name, args?, text?, raw, range } -> facts shape (text -> value).
function normAnnotations(linemap, list) {
    const out = [];
    if (!Array.isArray(list)) return out;
    for (const a of list) {
        // Field order mirrors frontend_lua.norm_annotations: { name, args?, value?, raw, range }.
        const rec = { name: a.name };
        if (a.args !== undefined) rec.args = a.args;
        if (a.text !== undefined) rec.value = a.text;    // rename text -> value
        rec.raw = a.raw;
        rec.range = annRange(linemap, a.range);
        out.push(rec);
    }
    return out;
}

// ---- declaration collection (full-AST walk; Lua parity) ----
// Walk a binding pattern, invoking cb(identifierNode, structuralBindingPath) for each bound name.
function collectPatternBindings(pat, path, cb) {
    if (!pat || typeof pat !== "object") return;
    switch (pat.type) {
        case "Identifier": cb(pat, path); return;
        case "ArrayPattern":
            for (let i = 0; i < (pat.elements || []).length; i++) {
                const el = pat.elements[i];
                if (el) collectPatternBindings(el, path.concat([{ array_index: i }]), cb);   // null = hole
            }
            return;
        case "ObjectPattern":
            for (let j = 0; j < (pat.properties || []).length; j++) {
                const p = pat.properties[j];
                if (!p) continue;
                if (p.type === "RestElement") collectPatternBindings(p.argument, path.concat([{ property_index: j }, { rest: true }]), cb);
                else collectPatternBindings(p.value, path.concat([{ property_index: j }]), cb);
            }
            return;
        case "AssignmentPattern": collectPatternBindings(pat.left, path.concat([{ assignment: true }]), cb); return;
        case "RestElement": collectPatternBindings(pat.argument, path.concat([{ rest: true }]), cb); return;
    }
}

function retainValueDecl(unit_id, kind, declNode, declarator_index, binding_path, name) {
    const decl_id = ++nextDeclId;
    decls.set(decl_id, { unit_id: unit_id, formKind: "value", kind: kind, node: declNode, declarator_index: declarator_index, binding_path: binding_path, name: name });
    return decl_id;
}
function retainSimpleDecl(unit_id, formKind, kind, node, name) {
    const decl_id = ++nextDeclId;
    decls.set(decl_id, { unit_id: unit_id, formKind: formKind, kind: kind, node: node, name: name });
    return decl_id;
}

// Pre-order walk over every AST node (a node is an object with a string `type`), collecting the
// declaration nodes at ANY nesting depth. Skips the annotation record keys (no `type`). Adds every
// retained declaration node to the unit's `nodes` set (for ownership validation). `inject` is a
// test-only fault marker ("midCollection" throws after the first retained decl).
function collectDeclarations(ast, unit_id, linemap, nodes, inject) {
    const out = [];
    function retained(node) { nodes.add(node); if (inject === "midCollection" && out.length >= 1) throw new Error("injected mid-collection failure"); }
    function visit(node) {
        if (!node || typeof node !== "object") return;
        const t = node.type;
        if (t === "VariableDeclaration") {
            const grp = nodeRange(linemap, node);
            const anns = normAnnotations(linemap, node.annotationList);
            for (let di = 0; di < (node.declarations || []).length; di++) {
                const d = node.declarations[di];
                if (!d || !d.id) continue;
                collectPatternBindings(d.id, [], function (idn, path) {
                    const decl_id = retainValueDecl(unit_id, node.kind, node, di, path, idn.name);
                    out.push({ kind: node.kind, name: idn.name, range: nodeRange(linemap, idn), group_range: grp,
                               is_method: false, annotations: anns, decl_id: decl_id });
                    retained(node);
                });
            }
        } else if (t === "FunctionDeclaration" && node.id && node.id.type === "Identifier") {
            const decl_id = retainSimpleDecl(unit_id, "function", "function", node, node.id.name);
            out.push({ kind: "function", name: node.id.name, range: nodeRange(linemap, node.id), group_range: nodeRange(linemap, node),
                       is_method: false, annotations: normAnnotations(linemap, node.annotationList), decl_id: decl_id });
            retained(node);
        } else if (t === "ClassDeclaration" && node.id && node.id.type === "Identifier") {
            const decl_id = retainSimpleDecl(unit_id, "class", "class", node, node.id.name);
            out.push({ kind: "class", name: node.id.name, range: nodeRange(linemap, node.id), group_range: nodeRange(linemap, node),
                       is_method: false, annotations: normAnnotations(linemap, node.annotationList), decl_id: decl_id });
            retained(node);
        }
        // recurse into children (an ExportNamed/DefaultDeclaration is not a declaration kind, so its
        // inner declaration is reached exactly once - no duplicate through the wrapper).
        for (const k in node) {
            if (k === "start" || k === "stop" || k === "type" || k === "annotations" || k === "annotationList") continue;
            const v = node[k];
            if (Array.isArray(v)) { for (const e of v) visit(e); }
            else if (v && typeof v === "object") visit(v);
        }
    }
    visit(ast);
    return out;
}

// ---- analyze (TRANSACTIONAL) ----
// On any internal failure, every unit/declaration this invocation added is deleted and the issued
// ids are NOT reused (counters stay monotonic), so no partial unit_id/decl_id remains resolvable.
function analyzeImpl(bytes, path, opts, inject) {
    const unitFloor = nextUnitId, declFloor = nextDeclId;
    try {
        const u = parse(bytes, { path: path || null });          // parser is never-raise
        const unit_id = ++nextUnitId;
        const nodes = new Set();
        units.set(unit_id, { ast: u.ast, comments: u.comments, linemap: u.linemap, nodes: nodes });
        const linemap = u.linemap;
        let hasError = false;
        const diagnostics = [];
        for (const d of u.diagnostics || []) {
            if (d.severity === "error") hasError = true;
            diagnostics.push({ severity: d.severity, code: d.code, message: d.message, range: d.range ? nrange(linemap, d.range.start, d.range.stop) : null });
        }
        const declarations = collectDeclarations(u.ast, unit_id, linemap, nodes, inject);
        return { schema_version: 1, status: hasError ? "error" : "analyzed", unit_id: unit_id, diagnostics: diagnostics, declarations: declarations };
    } catch (e) {
        for (let id = declFloor + 1; id <= nextDeclId; id++) decls.delete(id);   // roll back this call's decls
        for (let id = unitFloor + 1; id <= nextUnitId; id++) units.delete(id);   // and its unit(s)
        // counters stay advanced (monotonic): the rolled-back ids are never reissued.
        const msg = (e && e.message !== undefined) ? String(e.message) : String(e);
        return { schema_version: 1, status: "error", unit_id: -1, declarations: [],
                 diagnostics: [{ severity: "error", code: "js.internal", message: "frontend analyze failed: " + msg, range: null }] };
    }
}
export function analyze(bytes, path, opts) { return analyzeImpl(bytes, path, opts, null); }
// Test-only: force a mid-collection failure to prove analyze() is transactional (no production
// caller passes an injector). Not part of the shipped surface.
export function __analyzeWithFailure(bytes, path, opts) { return analyzeImpl(bytes, path, opts, "midCollection"); }

// ---- declaration_semantics ----
function semErr(msg) { return { error: { severity: "error", code: "js.internal", message: "declaration_semantics: " + msg, range: null } }; }
// Follow a structural binding_path from a declarator id pattern; returns the terminal node or null.
function followPath(node, path) {
    let cur = node;
    for (const step of path) {
        if (!cur || typeof cur !== "object") return null;
        if (step.array_index !== undefined) { if (cur.type !== "ArrayPattern") return null; cur = (cur.elements || [])[step.array_index]; }
        else if (step.property_index !== undefined) {
            if (cur.type !== "ObjectPattern") return null;
            const p = (cur.properties || [])[step.property_index];
            if (!p) return null;
            cur = (p.type === "RestElement") ? p : p.value;      // Property continues through .value; RestElement stays
        } else if (step.rest) { if (cur.type !== "RestElement") return null; cur = cur.argument; }
        else if (step.assignment) { if (cur.type !== "AssignmentPattern") return null; cur = cur.left; }
        else return null;
    }
    return cur;
}
export function declarationSemantics(declId) {
    try {
        const r = decls.get(declId);
        if (!r) return semErr("unknown declaration handle");
        const u = units.get(r.unit_id);
        if (!u) return semErr("declaration's unit is no longer retained");
        const node = r.node;
        // OWNERSHIP: the retained node must belong to THIS unit (not merely some unit with the id).
        if (!node || !u.nodes || !u.nodes.has(node)) return semErr("declaration node does not belong to its unit");
        if (r.formKind === "value") {
            if (node.type !== "VariableDeclaration") return semErr("node/kind mismatch: expected VariableDeclaration, got '" + node.type + "'");
            if (r.kind !== "const" && r.kind !== "let" && r.kind !== "var") return semErr("invalid value kind '" + r.kind + "'");
            if (r.kind !== node.kind) return semErr("kind mismatch: recorded '" + r.kind + "' vs node '" + node.kind + "'");
            const arr = node.declarations || [];
            if (typeof r.declarator_index !== "number" || r.declarator_index < 0 || r.declarator_index >= arr.length) return semErr("declarator index out of range");
            const d = arr[r.declarator_index];
            if (!d || !d.id) return semErr("malformed declarator");
            const term = followPath(d.id, r.binding_path || []);
            if (!term || term.type !== "Identifier" || term.name !== r.name) return semErr("binding path does not resolve to the recorded name");
            return { form: "value", kind: r.kind, declarator_index: r.declarator_index, binding_path: r.binding_path, initializer: d.init || null };
        } else if (r.formKind === "function") {
            if (r.kind !== "function") return semErr("invalid function kind '" + r.kind + "'");
            if (node.type !== "FunctionDeclaration") return semErr("node/kind mismatch: expected FunctionDeclaration, got '" + node.type + "'");
            if (!node.id || node.id.type !== "Identifier" || node.id.name !== r.name) return semErr("function name does not match the recorded name");
            if (!Array.isArray(node.params)) return semErr("malformed function declaration (params not a list)");
            if (!node.body || node.body.type !== "BlockStatement") return semErr("malformed function declaration (body not a BlockStatement)");
            return { form: "function", is_async: node.async === true, is_generator: node.generator === true, params: node.params, body: node.body };
        } else if (r.formKind === "class") {
            if (r.kind !== "class") return semErr("invalid class kind '" + r.kind + "'");
            if (node.type !== "ClassDeclaration") return semErr("node/kind mismatch: expected ClassDeclaration, got '" + node.type + "'");
            if (!node.id || node.id.type !== "Identifier" || node.id.name !== r.name) return semErr("class name does not match the recorded name");
            if (!Array.isArray(node.body)) return semErr("malformed class declaration (body not a list)");
            return { form: "class", super_class: node.superClass || null, body: node.body };
        }
        return semErr("unsupported declaration kind '" + r.formKind + "'");
    } catch (e) {
        const msg = (e && e.message !== undefined) ? String(e.message) : String(e);
        return semErr("internal fault: " + msg);
    }
}

// Test-only: mutate the RETAINED state of a declaration to exercise the corrupt-state boundary.
// spec = { declId, declField?, declValue?, nodeField?, nodeValue? }. Not part of the shipped
// surface (no production caller mutates retained state).
export function __mutate(spec) {
    if (!spec || typeof spec.declId !== "number") return { ok: false };
    const r = decls.get(spec.declId);
    if (!r) return { ok: false };
    if (Object.prototype.hasOwnProperty.call(spec, "declField")) r[spec.declField] = spec.declValue;
    if (Object.prototype.hasOwnProperty.call(spec, "nodeField") && r.node) r.node[spec.nodeField] = spec.nodeValue;
    return { ok: true };
}

// ---- scope capability (through the adapter) ----
export function scope(unitId) {
    try {
        const u = units.get(unitId);
        if (!u) return { ok: false, error: { severity: "error", code: "js.internal", message: "scope: unknown unit handle", range: null } };
        return scopeResolve(u);
    } catch (e) {
        const msg = (e && e.message !== undefined) ? String(e.message) : String(e);
        return { ok: false, error: { severity: "error", code: "js.internal", message: "scope: internal fault: " + msg, range: null } };
    }
}
