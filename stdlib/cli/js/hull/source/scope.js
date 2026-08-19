// hull:source:scope - structural lexical binding / name-resolution pass (JS mirror of
// hull.source.scope). Resolves every identifier REFERENCE to its binding (local / closure /
// global) and records, per binding, its kind + scope + reads + writes + shadowing. Operates on
// the AST + byte ranges ALONE (never executes application JS).
//
// resolve(unit) -> { ok:true, bindings:[Binding], refs:[Ref] }
//              or  { ok:false, error:{ code:"js.internal", message, range } }
//
// Binding = { name, kind, scope, funcId, scopeId, range, reads, writes, shadows|null, hoisted }
// Ref     = { name, range, kind:"local"|"closure"|"global", access:"read"|"write", declRange|null }
//
// Strict/module semantics (docs/js_frontend_slice4_scope.md): two-pass hoisting with an explicit
// function-wide var collection that stops at nested functions; params predeclared before defaults;
// loop-head bindings visible in their own RHS; deterministic redeclaration coalescence; compound
// access as a read ref then a write ref. TDZ + duplicate-binding + const-reassignment early errors
// are deliberately NOT enforced. NEVER raises: a recovered/error AST degrades locally; only an
// internal fault yields ok:false + one js.internal (never a partial ok:true).
//
// SPDX-License-Identifier: AGPL-3.0-or-later

const VARISH = { var: true, param: true, function: true };   // coalesce within a variable scope

export function resolve(unit) {
    try {
        const ast = unit && unit.ast;
        if (!ast || typeof ast !== "object" || ast.type !== "Program") {
            return { ok: false, error: { code: "js.internal", message: "scope resolver: missing Program AST", range: null } };
        }
        const st = { scopes: [], funcSeq: 0, scopeSeq: 0, bindings: [], refs: [] };
        resolveModule(st, ast);
        st.bindings.sort(function (a, b) { return (a.range.start - b.range.start) || (a.range.stop - b.range.stop); });
        return { ok: true, bindings: st.bindings, refs: st.refs };
    } catch (e) {
        const msg = (e && e.message !== undefined) ? String(e.message) : String(e);
        const r = (unit && unit.ast && typeof unit.ast.start === "number") ? { start: unit.ast.start, stop: unit.ast.stop } : null;
        return { ok: false, error: { code: "js.internal", message: "scope resolver failed: " + msg, range: r } };
    }
}

// ---- scope stack ----
function pushScope(st, kind, funcId) {
    const sc = { id: ++st.scopeSeq, kind: kind, funcId: funcId, names: Object.create(null) };
    st.scopes.push(sc);
    return sc;
}
function popScope(st) { st.scopes.pop(); }
function curScope(st) { return st.scopes[st.scopes.length - 1]; }
function curFuncId(st) { return curScope(st).funcId; }

// The nearest same-name binding in a STRICTLY ENCLOSING scope, or null (the shadow target).
function findShadow(st, name) {
    for (let i = st.scopes.length - 2; i >= 0; i--) { const b = st.scopes[i].names[name]; if (b) return b; }
    return null;
}

// Declare a binding in `scope` with deterministic redeclaration handling (docs 5.1).
function declare(st, scope, name, kind, range, hoisted) {
    if (!name || !range) return null;
    const existing = scope.names[name];
    if (existing) {
        if ((scope.kind === "function" || scope.kind === "module") && VARISH[existing.kind] && VARISH[kind]) {
            // legal coalescence: one binding, earliest-by-range wins name/kind/range; no new record.
            if (range.start < existing.range.start) { existing.kind = kind; existing.range = range; existing.hoisted = (kind === "var" || kind === "function"); }
            return existing;
        }
        // duplicate lexical (or a non-coalescible clash): keep a SEPARATE record for source fidelity,
        // but the lookup target stays the FIRST (do not overwrite scope.names).
        const dup = mkBinding(name, kind, scope, range, hoisted, findShadow(st, name));
        st.bindings.push(dup);
        return dup;
    }
    const b = mkBinding(name, kind, scope, range, hoisted, findShadow(st, name));
    scope.names[name] = b;
    st.bindings.push(b);
    return b;
}
function mkBinding(name, kind, scope, range, hoisted, shadows) {
    return { name: name, kind: kind, scope: scope.kind, funcId: scope.funcId, scopeId: scope.id,
             range: { start: range.start, stop: range.stop }, reads: 0, writes: 0,
             shadows: shadows ? { start: shadows.range.start, stop: shadows.range.stop } : null,
             hoisted: hoisted === true };
}

// Resolve one identifier reference (records the access + the ref).
function resolveRef(st, idNode, access) {
    if (!idNode || idNode.type !== "Identifier" || typeof idNode.name !== "string") return;
    const name = idNode.name;
    const myFunc = curFuncId(st);
    for (let i = st.scopes.length - 1; i >= 0; i--) {
        const b = st.scopes[i].names[name];
        if (b) {
            if (access === "write") b.writes++; else b.reads++;
            st.refs.push({ name: name, range: { start: idNode.start, stop: idNode.stop },
                           kind: (b.funcId === myFunc) ? "local" : "closure", access: access,
                           declRange: { start: b.range.start, stop: b.range.stop } });
            return;
        }
    }
    st.refs.push({ name: name, range: { start: idNode.start, stop: idNode.stop }, kind: "global", access: access, declRange: null });
}

// ---- binding-pattern name collection (no refs) ----
function patternNames(pat, cb) {
    if (!pat || typeof pat !== "object") return;
    switch (pat.type) {
        case "Identifier": cb(pat.name, pat); return;
        case "AssignmentPattern": patternNames(pat.left, cb); return;      // default: names on the left
        case "RestElement": patternNames(pat.argument, cb); return;
        case "ArrayPattern": for (const el of pat.elements || []) patternNames(el, cb); return;
        case "ObjectPattern":
            for (const p of pat.properties || []) {
                if (p.type === "RestElement") patternNames(p.argument, cb);
                else patternNames(p.value, cb);                            // Property.value is the binding target
            }
            return;
    }
}
// The READ parts of a binding pattern: default-value expressions + computed keys (resolve pass).
function patternReads(st, pat) {
    if (!pat || typeof pat !== "object") return;
    switch (pat.type) {
        case "AssignmentPattern": patternReads(st, pat.left); resolveExpr(st, pat.right); return;   // default value is a read
        case "RestElement": patternReads(st, pat.argument); return;
        case "ArrayPattern": for (const el of pat.elements || []) patternReads(st, el); return;
        case "ObjectPattern":
            for (const p of pat.properties || []) {
                if (p.type === "RestElement") { patternReads(st, p.argument); continue; }
                if (p.computed) resolveExpr(st, p.key);
                patternReads(st, p.value);
            }
            return;
    }
}

// ---- function-wide var + top-level function collection (stops at nested functions) ----
function isFn(t) { return t === "FunctionDeclaration" || t === "FunctionExpression" || t === "ArrowFunctionExpression"; }
function declareVarsAndTopFns(st, bodyStmts, varScope) {
    for (const s of bodyStmts) if (s && s.type === "FunctionDeclaration" && s.id) declare(st, varScope, s.id.name, "function", s.id, true);
    // recursive var collection over the whole body subtree, stopping at nested function boundaries
    function rec(node) {
        if (!node || typeof node !== "object") return;
        if (isFn(node.type)) return;                                        // do not descend into nested functions
        if (node.type === "VariableDeclaration" && node.kind === "var") {
            for (const d of node.declarations || []) patternNames(d.id, function (nm, idn) { declare(st, varScope, nm, "var", idn, true); });
        }
        for (const k in node) {
            if (k === "type" || k === "start" || k === "stop") continue;
            const v = node[k];
            if (Array.isArray(v)) { for (const e of v) rec(e); }
            else if (v && typeof v === "object") rec(v);
        }
    }
    for (const s of bodyStmts) rec(s);
}
// Predeclare a block's DIRECT lexical children. includeFns=true for a nested block (block-level
// function declarations are block-scoped); false for a function body (its top-level functions
// already went to the function scope).
function declareBlockLexical(st, stmts, blockScope, includeFns) {
    for (const s of stmts) {
        if (!s || typeof s !== "object") continue;
        if (s.type === "VariableDeclaration" && (s.kind === "let" || s.kind === "const")) {
            for (const d of s.declarations || []) patternNames(d.id, function (nm, idn) { declare(st, blockScope, nm, s.kind, idn, false); });
        } else if (s.type === "ClassDeclaration" && s.id) {
            declare(st, blockScope, s.id.name, "class", s.id, false);
        } else if (includeFns && s.type === "FunctionDeclaration" && s.id) {
            declare(st, blockScope, s.id.name, "function", s.id, false);
        }
    }
}

// ---- scope drivers ----
function resolveModule(st, program) {
    const mod = pushScope(st, "module", 0);
    const body = program.body || [];
    for (const s of body) if (s && s.type === "ImportDeclaration") declareImport(st, s, mod);
    declareVarsAndTopFns(st, body, mod);
    declareBlockLexical(st, body, mod, false);                              // top-level let/const/class
    for (const s of body) resolveStmt(st, s);
    popScope(st);
}
function declareImport(st, node, mod) {
    for (const sp of node.specifiers || []) {
        const local = sp.local || sp.imported || sp.exported;
        if (local && local.type === "Identifier") declare(st, mod, local.name, "import", local, true);
    }
}

// A function/arrow/method value. selfNode = a named FunctionExpression whose name is body-only.
function resolveFunction(st, node, selfNode) {
    const fn = pushScope(st, "function", ++st.funcSeq);
    if (selfNode && selfNode.id && selfNode.id.type === "Identifier") declare(st, fn, selfNode.id.name, "function", selfNode.id, false);
    const params = node.params || [];
    for (const p of params) patternNames(p, function (nm, idn) { declare(st, fn, nm, "param", idn, false); });
    const bodyIsBlock = node.body && node.body.type === "BlockStatement";
    if (bodyIsBlock) declareVarsAndTopFns(st, node.body.body || [], fn);
    for (const p of params) patternReads(st, p);                           // default values, computed keys (params visible)
    if (bodyIsBlock) {
        const blk = pushScope(st, "block", fn.funcId);
        declareBlockLexical(st, node.body.body || [], blk, false);         // body top-level let/const/class only
        for (const s of node.body.body || []) resolveStmt(st, s);
        popScope(st);
    } else if (node.body) {
        resolveExpr(st, node.body);                                        // arrow expression body
    }
    popScope(st);
}

function resolveClass(st, node, isExpr) {
    if (node.superClass) resolveExpr(st, node.superClass);                 // extends B: read in the enclosing scope
    let pushed = false;
    if (isExpr && node.id && node.id.type === "Identifier") {              // class-expression name is body-only
        const cs = pushScope(st, "block", curFuncId(st));
        declare(st, cs, node.id.name, "class", node.id, false);
        pushed = true;
    }
    for (const m of node.body || []) {
        if (!m || typeof m !== "object") continue;
        if (m.computed) resolveExpr(st, m.key);                            // computed member key is a read
        if (m.value) {
            if (m.value.type === "FunctionExpression" || m.value.type === "ArrowFunctionExpression") resolveFunction(st, m.value, null);
            else resolveExpr(st, m.value);                                 // field initializer
        }
    }
    if (pushed) popScope(st);
}

// ---- statements ----
function resolveStmt(st, s) {
    if (!s || typeof s !== "object") return;
    switch (s.type) {
        case "ExpressionStatement": resolveExpr(st, s.expression); return;
        case "VariableDeclaration":
            for (const d of s.declarations || []) { if (d) { patternReads(st, d.id); if (d.init) resolveExpr(st, d.init); } }
            return;
        case "FunctionDeclaration": resolveFunction(st, s, null); return;   // name already bound
        case "ClassDeclaration": resolveClass(st, s, false); return;
        case "BlockStatement": {
            const blk = pushScope(st, "block", curFuncId(st));
            declareBlockLexical(st, s.body || [], blk, true);
            for (const x of s.body || []) resolveStmt(st, x);
            popScope(st);
            return;
        }
        case "IfStatement": resolveExpr(st, s.test); resolveStmt(st, s.consequent); if (s.alternate) resolveStmt(st, s.alternate); return;
        case "WhileStatement": resolveExpr(st, s.test); resolveStmt(st, s.body); return;
        case "DoWhileStatement": resolveStmt(st, s.body); resolveExpr(st, s.test); return;
        case "ForStatement": resolveForClassic(st, s); return;
        case "ForOfStatement": case "ForInStatement": resolveForInOf(st, s); return;
        case "SwitchStatement": resolveSwitch(st, s); return;
        case "TryStatement": resolveTry(st, s); return;
        case "ReturnStatement": if (s.argument) resolveExpr(st, s.argument); return;
        case "ThrowStatement": if (s.argument) resolveExpr(st, s.argument); return;
        case "LabeledStatement": resolveStmt(st, s.body); return;          // label is a separate namespace
        case "ImportDeclaration": return;                                  // bindings predeclared
        case "ExportNamedDeclaration":
            if (s.declaration) resolveStmt(st, s.declaration);
            else for (const sp of s.specifiers || []) { const loc = sp.local; if (loc && loc.type === "Identifier") resolveRef(st, loc, "read"); }
            return;
        case "ExportDefaultDeclaration":
            if (s.declaration && s.declaration.type === "FunctionDeclaration") resolveFunction(st, s.declaration, null);
            else if (s.declaration && s.declaration.type === "ClassDeclaration") resolveClass(st, s.declaration, false);
            else if (s.declaration) resolveExpr(st, s.declaration);
            return;
        // BreakStatement / ContinueStatement / EmptyStatement / Error / DebuggerStatement: nothing
    }
}

function resolveForClassic(st, s) {
    const lexical = s.init && s.init.type === "VariableDeclaration" && (s.init.kind === "let" || s.init.kind === "const");
    let pushed = false;
    if (lexical) {
        const head = pushScope(st, "loop-head", curFuncId(st));            // head bindings visible in the whole head
        for (const d of s.init.declarations || []) patternNames(d.id, function (nm, idn) { declare(st, head, nm, s.init.kind, idn, false); });
        pushed = true;
    }
    if (s.init) { if (s.init.type === "VariableDeclaration") resolveStmt(st, s.init); else resolveExpr(st, s.init); }
    if (s.test) resolveExpr(st, s.test);
    if (s.update) resolveExpr(st, s.update);
    resolveStmt(st, s.body);
    if (pushed) popScope(st);
}
function resolveForInOf(st, s) {
    const lexical = s.left && s.left.type === "VariableDeclaration" && (s.left.kind === "let" || s.left.kind === "const");
    let pushed = false;
    if (lexical) {
        const head = pushScope(st, "loop-head", curFuncId(st));
        for (const d of s.left.declarations || []) patternNames(d.id, function (nm, idn) { declare(st, head, nm, s.left.kind, idn, false); });
        pushed = true;
    }
    // RHS resolves WITH the loop binding visible (for (let x of x) -> x binds the loop var)
    if (s.right) resolveExpr(st, s.right);
    if (s.left) {
        if (s.left.type === "VariableDeclaration") { for (const d of s.left.declarations || []) patternReads(st, d.id); }
        else resolveAssignTarget(st, s.left);                              // for (x of ...) with no declaration -> write
    }
    resolveStmt(st, s.body);
    if (pushed) popScope(st);
}
function resolveSwitch(st, s) {
    resolveExpr(st, s.discriminant);
    const blk = pushScope(st, "block", curFuncId(st));                     // ONE shared block for all cases
    for (const c of s.cases || []) declareBlockLexical(st, c.consequent || [], blk, true);
    for (const c of s.cases || []) {
        if (c.test) resolveExpr(st, c.test);
        for (const x of c.consequent || []) resolveStmt(st, x);
    }
    popScope(st);
}
function resolveTry(st, s) {
    if (s.block) resolveStmt(st, s.block);
    const h = s.handler;
    if (h) {
        if (h.param) {
            const cs = pushScope(st, "catch", curFuncId(st));
            patternNames(h.param, function (nm, idn) { declare(st, cs, nm, "catch", idn, false); });
            patternReads(st, h.param);
            resolveStmt(st, h.body);
            popScope(st);
        } else if (h.body) {
            resolveStmt(st, h.body);
        }
    }
    if (s.finalizer) resolveStmt(st, s.finalizer);
}

// ---- expressions ----
function resolveExpr(st, node) {
    if (!node || typeof node !== "object") return;
    switch (node.type) {
        case "Identifier": resolveRef(st, node, "read"); return;
        case "MemberExpression":
            resolveExpr(st, node.object);
            if (node.computed) resolveExpr(st, node.property);             // a.b -> b is a key, not a ref
            return;
        case "CallExpression": case "NewExpression":
            resolveExpr(st, node.callee);
            for (const a of node.arguments || []) resolveExpr(st, a);
            return;
        case "AssignmentExpression": resolveAssignment(st, node); return;
        case "UpdateExpression":
            if (node.argument && node.argument.type === "Identifier") { resolveRef(st, node.argument, "read"); resolveRef(st, node.argument, "write"); }
            else resolveExpr(st, node.argument);
            return;
        case "BinaryExpression": case "LogicalExpression": resolveExpr(st, node.left); resolveExpr(st, node.right); return;
        case "UnaryExpression": case "AwaitExpression": case "YieldExpression": case "SpreadElement":
            resolveExpr(st, node.argument); return;
        case "ConditionalExpression": resolveExpr(st, node.test); resolveExpr(st, node.consequent); resolveExpr(st, node.alternate); return;
        case "SequenceExpression": for (const e of node.expressions || []) resolveExpr(st, e); return;
        case "ArrayExpression": for (const e of node.elements || []) resolveExpr(st, e); return;
        case "ObjectExpression":
            for (const p of node.properties || []) {
                if (p.type === "SpreadElement") { resolveExpr(st, p.argument); continue; }
                if (p.computed) resolveExpr(st, p.key);                    // non-computed key is a property name, not a ref
                resolveExpr(st, p.value);                                  // shorthand { x }: value IS the Identifier -> a read
            }
            return;
        case "TemplateLiteral": for (const e of node.expressions || []) resolveExpr(st, e); return;
        case "TaggedTemplateExpression": resolveExpr(st, node.tag); resolveExpr(st, node.quasi); return;
        case "ArrowFunctionExpression": resolveFunction(st, node, null); return;
        case "FunctionExpression": resolveFunction(st, node, node); return;   // self-name is body-only
        case "ClassExpression": resolveClass(st, node, true); return;
        case "ImportExpression": resolveExpr(st, node.source); return;
        // Literal / ThisExpression / Super / MetaProperty / Error: no refs
    }
}
function resolveAssignment(st, node) {
    const left = node.left;
    if (node.operator === "=") {
        if (left && left.type === "Identifier") resolveRef(st, left, "write");
        else if (left && (left.type === "ArrayPattern" || left.type === "ObjectPattern")) resolveAssignTarget(st, left);
        else resolveExpr(st, left);                                        // member target: object/computed reads
    } else {                                                               // compound: read then write
        if (left && left.type === "Identifier") { resolveRef(st, left, "read"); resolveRef(st, left, "write"); }
        else resolveExpr(st, left);
    }
    resolveExpr(st, node.right);
}
// A destructuring ASSIGNMENT target (not a declaration): each name is a write; defaults/computed are reads.
function resolveAssignTarget(st, node) {
    if (!node || typeof node !== "object") return;
    switch (node.type) {
        case "Identifier": resolveRef(st, node, "write"); return;
        case "MemberExpression": resolveExpr(st, node); return;
        case "AssignmentPattern": resolveAssignTarget(st, node.left); resolveExpr(st, node.right); return;
        case "RestElement": resolveAssignTarget(st, node.argument); return;
        case "ArrayPattern": for (const el of node.elements || []) resolveAssignTarget(st, el); return;
        case "ObjectPattern":
            for (const p of node.properties || []) {
                if (p.type === "RestElement") { resolveAssignTarget(st, p.argument); continue; }
                if (p.computed) resolveExpr(st, p.key);
                resolveAssignTarget(st, p.value);
            }
            return;
        default: resolveExpr(st, node);
    }
}
