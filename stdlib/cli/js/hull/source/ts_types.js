// hull:source:ts_types - the TypeScript type grammar for the parser's TypeScript mode
// (docs/typescript_design.md section 3).
//
// Types build no AST: erasure only needs to know where a type ends, so the grammar consumes
// the tokens of one type and the caller records the span (`: T`, `as T`, ...). It is a factory
// over the parser's token interface, so it shares the parser's tokenizer, lookahead, depth
// limit and diagnostic budget:
//
//   createTypeGrammar(p) -> { parseType(opts?), parseReturnType(), parseTypeParameters(),
//                             parseTypeArguments(), parseParameters(), parseObjectType(), startsType() }
//   p: { cur(), advance(goal), peek(k), isP(v), isKw(v), nl(), splitGreater(), synErr(msg, tok),
//        guard(), unguard(), refuse(msg, tok) }
//
// Every token inside a type is read with the operator slash goal (no type holds a regex), and
// so is the first token after it: everywhere a type ends, JavaScript would read the next token
// after an identifier or a `)`. A malformed type is a js.syntax error ("expected a type") and
// consumes nothing more, leaving the JS parser to recover.
//
// Covered: type references with type arguments (a `>>` closing two lists is split), qualified
// names, `typeof` queries, `import("m").T`, keyword and literal types (string, number, `-1`,
// template literal types, `true` / `false` / `null` / `undefined` / `void` / `this`), unions and
// intersections (leading `|` / `&` included), `keyof` / `unique` / `readonly` / `infer`, arrays,
// indexed access, tuples (named, optional, rest), object types (properties, methods, call and
// construct signatures, index signatures, get / set, `readonly`), mapped types (`as`, `+` / `-`
// modifiers), conditional types, function and constructor types (`abstract new`), generic
// function types, type predicates (`x is T`, `asserts x`, `asserts x is T`), and `as const`.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

export function createTypeGrammar(p) {
    function cur() { return p.cur(); }
    function adv() { return p.advance(false); }
    function isP(v) { return p.isP(v); }
    function isKw(v) { return p.isKw(v); }
    function isId(t) { return t.type === "identifier" && !t.escaped; }
    function peekP(k, v) { const t = p.peek(k); return t.type === "punctuator" && t.value === v; }
    function expect(v) { if (isP(v)) { adv(); return true; } p.synErr("expected '" + v + "' in a type"); return false; }
    function fail() { p.synErr("expected a type"); return false; }

    // A `>` closing a type argument or parameter list; `>>`, `>=`, ... give up their first `>`.
    function closeAngle() {
        const t = cur();
        if (t.type === "punctuator" && t.value.length > 1 && t.value.charCodeAt(0) === 0x3e) p.splitGreater();
        return expect(">");
    }

    // Does the current token start a type? (A type cannot start with anything else, so a caller
    // that sees false reports "expected a type" without consuming.)
    function startsType() {
        const t = cur();
        if (t.type === "identifier" || t.type === "string" || t.type === "number" || t.type === "template") return true;
        return t.type === "punctuator" && (t.value === "(" || t.value === "[" || t.value === "{" || t.value === "<" ||
            t.value === "|" || t.value === "&" || t.value === "-");
    }

    // A comma-separated list up to `close`, each item read by `item`. Stops on an item that
    // consumed nothing, so a malformed list never loops.
    function list(close, item) {
        while (!isP(close) && cur().type !== "eof") {
            const before = cur().start;
            item();
            if (cur().start === before) return false;
            if (!isP(",")) break;
            adv();
        }
        return true;
    }

    // A `/` (or a regex) inside a bracket group the type grammar skips: every token there is
    // lexed with the operator slash goal, where TypeScript reads an expression (a computed key
    // `[/]/.source]`) and so a regular expression - the skip would end at a bracket inside it,
    // and run what TypeScript erases. No type holds a `/`, so it is refused (audit 50).
    function isSlash(t) { return t.type === "regex" || (t.type === "punctuator" && (t.value === "/" || t.value === "/=")); }

    // Skip one balanced bracket group starting at the current `(`, `[` or `{` (a destructuring
    // pattern standing for a parameter name in a function type, or a computed property name).
    function skipBalanced() {
        let depth = 0;
        do {
            const t = cur();
            if (t.type === "eof") return;
            if (isSlash(t)) { p.refuse("a '/' inside a type is not supported", t); return; }
            if (t.type === "punctuator") {
                if (t.value === "(" || t.value === "[" || t.value === "{") depth++;
                else if (t.value === ")" || t.value === "]" || t.value === "}") depth--;
            }
            adv();
        } while (depth > 0);
    }

    // Peek past the balanced group opening at lookahead position k; returns the position of the
    // token after it, or 0 when the input ends first.
    function peekPastGroup(k) {
        let depth = 0;
        for (;;) {
            const t = p.peek(k);
            if (t.type === "eof" || isSlash(t)) return 0;
            if (t.type === "punctuator") {
                if (t.value === "(" || t.value === "[" || t.value === "{") depth++;
                else if (t.value === ")" || t.value === "]" || t.value === "}") { depth--; if (depth === 0) return k + 1; }
            }
            k++;
        }
    }

    // `(` at the current token starts a function type `(params) => R` rather than a
    // parenthesized type: `()`, `(...`, `(name:` / `(name?` / `(name,` / `(name =`, `(name) =>`,
    // `(this:`, or a pattern `({ a }) =>`.
    function parenStartsFunctionType() {
        const t1 = p.peek(1);
        if (t1.type === "punctuator") {
            if (t1.value === ")" || t1.value === "...") return true;
            if (t1.value === "{" || t1.value === "[") { const k = peekPastGroup(1); return k > 0 && (peekP(k, ":") || peekP(k, ",") || peekP(k, "?") || (peekP(k, ")") && peekP(k + 1, "=>"))); }
            return false;
        }
        if (t1.type !== "identifier") return false;
        if (peekP(2, ":") || peekP(2, ",") || peekP(2, "?") || peekP(2, "=")) return true;
        return peekP(2, ")") && peekP(3, "=>");
    }

    // A function type's parameter list: `(a: T, b?: U, ...c: V, { d }: W, this: X)`.
    function parameters() {
        if (!expect("(")) return;
        list(")", function () {
            if (isP("...")) adv();
            if (isP("{") || isP("[")) skipBalanced();
            else if (isId(cur())) adv();
            else { fail(); return; }
            if (isP("?")) adv();
            if (isP(":")) { adv(); parseType(); }
        });
        expect(")");
    }

    // `<T, const U extends V = W, in out X>` on a function type, method or call signature.
    function parseTypeParameters() {
        if (!expect("<")) return;
        list(">", function () {
            while ((isKw("const") || isKw("in") || isKw("out")) && isId(p.peek(1))) adv();
            if (!isId(cur())) { fail(); return; }
            adv();
            if (isKw("extends")) { adv(); parseType(); }
            if (isP("=")) { adv(); parseType(); }
        });
        closeAngle();
    }

    function typeArguments() {
        adv();                                          // past `<`
        list(">", function () { parseType(); });
        closeAngle();
    }

    // A return type, which may be a type predicate: `x is T`, `this is T`, `asserts x`,
    // `asserts x is T`.
    function parseReturnType() {
        if (isKw("asserts") && isId(p.peek(1)) && !p.peek(1).nlBefore) {
            adv(); adv();
            if (isKw("is") && !p.nl()) { adv(); parseType(); }
            return;
        }
        if (isId(cur()) && p.peek(1).type === "identifier" && p.peek(1).value === "is" && !p.peek(1).nlBefore) { adv(); adv(); parseType(); return; }
        parseType();
    }

    function functionType() {
        if (isP("<")) parseTypeParameters();
        parameters();
        expect("=>");
        parseReturnType();
    }

    // opts.noConditional: inside a conditional type's `extends` clause.
    // opts.asConst: `as const` (the caller is an `as`).
    function parseType(opts) {
        if (!p.guard()) { p.unguard(); return; }
        const noCond = !!(opts && opts.noConditional);
        if (isP("<") || (isP("(") && parenStartsFunctionType())) { functionType(); p.unguard(); return; }
        if (isKw("new") || (isKw("abstract") && p.peek(1).type === "identifier" && p.peek(1).value === "new")) {
            if (isKw("abstract")) adv();
            adv(); functionType(); p.unguard(); return;
        }
        if (opts && opts.asConst && isKw("const")) { adv(); p.unguard(); return; }
        union();
        if (!noCond && isKw("extends") && !p.nl()) {
            adv(); parseType({ noConditional: true });
            if (expect("?")) { parseType(); if (expect(":")) parseType(); }
        }
        p.unguard();
    }

    function union() {
        if (isP("|")) adv();
        intersection();
        while (isP("|")) { adv(); intersection(); }
    }
    function intersection() {
        if (isP("&")) adv();
        operator();
        while (isP("&")) { adv(); operator(); }
    }
    function operator() {
        if ((isKw("keyof") || isKw("unique") || isKw("readonly")) && startsAfter()) { adv(); if (!p.guard()) { p.unguard(); return; } operator(); p.unguard(); return; }
        if (isKw("infer") && isId(p.peek(1))) {
            adv(); adv();
            // `infer U extends C` constrains U only where a `?` cannot follow (else it is the
            // enclosing conditional type's `extends`).
            return;
        }
        postfix();
    }
    // The token after a type operator keyword starts a type (else the keyword is a type name).
    function startsAfter() {
        const t = p.peek(1);
        if (t.type === "identifier" || t.type === "string" || t.type === "number" || t.type === "template") return true;
        return t.type === "punctuator" && (t.value === "(" || t.value === "[" || t.value === "{" || t.value === "-");
    }
    function postfix() {
        if (!primary()) return;
        while (isP("[") && !p.nl()) {
            adv();
            if (isP("]")) { adv(); continue; }
            parseType();
            expect("]");
        }
    }

    function primary() {
        const t = cur();
        if (t.type === "string" || t.type === "number") { adv(); return true; }
        if (t.type === "template") { templateType(); return true; }
        if (t.type === "punctuator") {
            switch (t.value) {
                case "-": if (p.peek(1).type === "number") { adv(); adv(); return true; } return fail();
                case "(": adv(); parseType(); expect(")"); return true;
                case "[": tupleType(); return true;
                case "{": objectOrMappedType(); return true;
                default: return fail();
            }
        }
        if (t.type !== "identifier") return fail();
        if (t.value === "typeof" && !t.escaped) {
            adv();
            if (isKw("import")) importType(); else entityName();
            if (isP("<") && !p.nl()) typeArguments();
            return true;
        }
        if (t.value === "import" && !t.escaped && peekP(1, "(")) { importType(); if (isP("<") && !p.nl()) typeArguments(); return true; }
        entityName();
        if (isP("<") && !p.nl()) typeArguments();
        return true;
    }
    // `A`, `A.B.C` (any identifier name after a dot).
    function entityName() {
        if (cur().type !== "identifier") return fail();
        adv();
        while (isP(".") && p.peek(1).type === "identifier") { adv(); adv(); }
        return true;
    }
    // `import("m")`, `import("m").A.B`.
    function importType() {
        adv();                                          // past `import`
        if (!expect("(")) return;
        if (cur().type === "string") adv(); else fail();
        expect(")");
        while (isP(".") && p.peek(1).type === "identifier") { adv(); adv(); }
    }
    // `\`a${T}b${U}c\``: the lexer reads the template pieces; a type fills each hole.
    function templateType() {
        const t = cur();
        adv();
        if (t.mode === "noSubstitution") return;
        for (;;) {
            const before = cur().start;
            parseType();
            const m = cur();
            if (m.type !== "template" || (m.mode !== "middle" && m.mode !== "tail")) { if (cur().start === before) fail(); return; }
            adv();
            if (m.mode === "tail") return;
        }
    }
    // `[A, b: B, c?: C, D?, ...E[]]`
    function tupleType() {
        adv();                                          // past `[`
        list("]", function () {
            if (isP("...")) adv();
            if (isId(cur()) && (peekP(1, ":") || (peekP(1, "?") && peekP(2, ":")))) {
                adv(); if (isP("?")) adv(); adv();
            }
            parseType();
            if (isP("?")) adv();
        });
        expect("]");
    }

    // `{ ... }`: a mapped type (`[K in T]` as its member) or an object type.
    function objectOrMappedType() {
        adv();                                          // past `{`
        while (!isP("}") && cur().type !== "eof") {
            const before = cur().start;
            member();
            if (isP(",") || isP(";")) adv();
            else if (!isP("}") && !cur().nlBefore) { p.synErr("expected ';' or ',' in a type"); return; }
            if (cur().start === before) return;
        }
        expect("}");
    }
    function propertyName() {
        const t = cur();
        if (t.type === "identifier" || t.type === "string" || t.type === "number") { adv(); return true; }
        if (isP("[")) { skipBalanced(); return true; }
        return fail();
    }
    // A member's tail after its name: `?`, then a method signature `(...)` / `<T>(...)` with an
    // optional return type, or a property type.
    function memberTail() {
        if (isP("?")) adv();
        if (isP("(") || isP("<")) {
            if (isP("<")) parseTypeParameters();
            parameters();
            if (isP(":")) { adv(); parseReturnType(); }
            return;
        }
        if (isP(":")) { adv(); parseType(); }
    }
    function member() {
        // Call / construct signatures.
        if (isP("(") || isP("<")) { if (isP("<")) parseTypeParameters(); parameters(); if (isP(":")) { adv(); parseReturnType(); } return; }
        if (isKw("new") && (peekP(1, "(") || peekP(1, "<"))) { adv(); if (isP("<")) parseTypeParameters(); parameters(); if (isP(":")) { adv(); parseReturnType(); } return; }
        // Modifiers: `+readonly` / `-readonly` (mapped), `readonly`, and `get` / `set` accessors.
        if ((isP("+") || isP("-")) && p.peek(1).type === "identifier" && p.peek(1).value === "readonly") adv();
        if (isKw("readonly") && !peekP(1, "?") && !peekP(1, ":") && !peekP(1, "(") && !peekP(1, ",") && !peekP(1, ";") && !peekP(1, "}")) adv();
        if ((isKw("get") || isKw("set")) && !peekP(1, "?") && !peekP(1, ":") && !peekP(1, "(") && !peekP(1, ",") && !peekP(1, ";") && !peekP(1, "}") && !peekP(1, "<")) adv();
        // Index signature `[k: T]: U` and mapped type `[K in T as N]+?: U`.
        if (isP("[") && isId(p.peek(1)) && (peekP(2, ":") || (p.peek(2).type === "identifier" && p.peek(2).value === "in"))) {
            const mapped = !peekP(2, ":");
            adv(); adv(); adv();                        // `[`, the name, `:` or `in`
            parseType();
            if (mapped && isKw("as")) { adv(); parseType(); }
            expect("]");
            if ((isP("+") || isP("-")) && peekP(1, "?")) adv();
            if (isP("?")) adv();
            if (isP(":")) { adv(); parseType(); }
            return;
        }
        if (!propertyName()) return;
        memberTail();
    }

    // `<A, B>` type arguments (cur is `<`), as in a type reference or after an expression.
    function parseTypeArguments() { if (isP("<")) typeArguments(); else p.synErr("expected '<'"); }

    return { parseType: parseType, parseReturnType: parseReturnType, parseTypeParameters: parseTypeParameters,
             parseTypeArguments: parseTypeArguments, parseParameters: parameters, parseObjectType: objectOrMappedType,
             startsType: startsType };
}
