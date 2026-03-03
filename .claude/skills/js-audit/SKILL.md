---
name: js-audit
description: Audit JavaScript stdlib code for security, correctness, and sandbox safety. Use when reviewing or hardening JS modules.
user-invocable: true
---

# JavaScript Code Audit Skill

Perform comprehensive security, correctness, and quality audits on Hull JavaScript stdlib code.

**Target:** $ARGUMENTS (default: all `stdlib/js/hull/*.js` files)

## Usage

```
/js-audit                               # Audit all JS stdlib modules
/js-audit stdlib/js/hull/template.js    # Audit a specific module
/js-audit --fix                         # Audit and apply fixes
```

## Hull JS Context

Hull JS code runs inside a sandboxed QuickJS (ES2023) interpreter:
- **Removed globals:** `eval()` disabled at C level
- **Not loaded:** `std`, `os` modules
- **Memory limit:** 64 MB (`JS_SetMemoryLimit`)
- **Stack limit:** 1 MB (`JS_SetMaxStackSize`)
- **Gas metering:** Instruction-count interrupt handler
- **Module system:** Only `hull:*` modules available via `import`
- **C capabilities:** `db`, `crypto`, `time`, `env`, `fs`, `http` — accessed through hull module imports

## Audit Categories

### 1. Sandbox Safety (Critical)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Sandbox escape | Use of `eval()`, `Function()` constructor, `new Function()` | Critical |
| Unsafe eval | String-to-code conversion outside C bridge `_template.compile()` | Critical |
| Module smuggling | `import` of non-hull modules | Critical |
| Global pollution | Writing to `globalThis` or undeclared variables (non-strict) | High |
| Prototype pollution | Modifying `Object.prototype`, `Array.prototype`, etc. | Critical |
| Symbol abuse | Using `Symbol.toPrimitive` or `Symbol.hasInstance` to bypass checks | Medium |
| Proxy trap abuse | Using `Proxy` objects to intercept capability calls | High |

**Hull-specific:** The template engine's `_template.compile()` uses `JS_Eval` via C bridge — this is the ONLY allowed code compilation path. Verify no JS-level code compilation exists.

### 2. Input Validation & Injection

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| SQL injection | String concatenation/template literals in SQL queries | Critical |
| XSS via template | Unescaped user data in template output | High |
| Path traversal | Unsanitized paths passed to `fs.*` | High |
| Header injection | `\r\n` in HTTP header values | High |
| Command injection | User input in `tool.spawn()` arguments | Critical |
| Timing attack | Non-constant-time string comparison of secrets/tokens | High |
| ReDoS | Unbounded regex on user input | Medium |

**SQL safety check:**
```javascript
// BAD: template literal interpolation
db.query(`SELECT * FROM users WHERE id = ${id}`);

// BAD: string concatenation
db.query("SELECT * FROM users WHERE id = " + id);

// GOOD: parameterized
db.query("SELECT * FROM users WHERE id = ?", [id]);
```

**Template safety check:**
```javascript
// BAD: raw output of user data
template.renderString("{{{ user_input }}}", data);

// GOOD: auto-escaped output
template.renderString("{{ user_input }}", data);
```

### 3. Error Handling

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Unchecked null/undefined | Property access on potentially null value | High |
| Swallowed exceptions | `try/catch` that discards error | Medium |
| Missing error propagation | Error condition not thrown or returned | Medium |
| Bare `throw` | Throwing non-Error objects (strings, numbers) | Low |
| Unhandled promise rejection | Async operations without `.catch()` or try/catch | Medium |
| Missing return after error | Function continues after error condition | High |

**Patterns to check:**
```javascript
// BAD: unchecked
const result = db.query("SELECT * FROM users WHERE id = ?", [id]);
const name = result[0].name;  // crashes if result is empty

// GOOD: null-safe
const result = db.query("SELECT * FROM users WHERE id = ?", [id]);
if (!result || result.length === 0) return null;
const name = result[0].name;
```

### 4. Type Safety

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Missing type checks | Function params not validated with `typeof` | Medium |
| Loose equality | Using `==` instead of `===` | Medium |
| Null vs undefined confusion | Not distinguishing `null` from `undefined` | Low |
| NaN propagation | Arithmetic on non-numbers without `isNaN()` check | Medium |
| Implicit coercion | `+` operator on mixed types (string + number) | Medium |
| Array method on non-array | `.map()`, `.filter()` on potentially non-array values | Medium |

**JS-specific pitfalls:**
```javascript
// BAD: loose equality
if (value == null)  // matches both null and undefined

// GOOD: explicit
if (value === null || value === undefined)

// BAD: implicit coercion
const total = count + "items"  // "5items" not "5 items"

// GOOD: explicit
const total = `${count} items`
```

### 5. Resource Management

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Unbounded Map/Set growth | Collections that grow without limit | High |
| Missing cache eviction | Caches without TTL or size limit | Medium |
| Closure leaks | Closures capturing large objects unnecessarily | Medium |
| String concatenation in loops | `s += chunk` in tight loops | Medium |
| Large intermediate arrays | Building arrays that could exceed memory limit | Medium |
| WeakRef/FinalizationRegistry | Not available in QuickJS — don't rely on them | Low |

**Performance patterns:**
```javascript
// BAD: O(n^2) string building
let s = "";
for (const item of items) {
    s += item;  // copies entire string each iteration
}

// GOOD: array join
const parts = [];
for (const item of items) {
    parts.push(item);
}
const s = parts.join("");
```

### 6. Crypto & Auth Safety

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Hardcoded secrets | Literal strings used as HMAC/JWT secrets | Critical |
| Weak secrets | Short or predictable secret values | High |
| Timing attacks | `===` comparison on HMAC digests or tokens | High |
| Missing expiry | Tokens/sessions without TTL | Medium |
| Insecure defaults | `secure` flag missing on cookies, `httpOnly` not set | Medium |
| Nonce reuse | Same nonce/IV used for multiple encryptions | Critical |

**Constant-time comparison:**
```javascript
// BAD: early-exit comparison
if (token === expected) { ... }

// GOOD: use crypto.verifyPassword or HMAC-then-compare
// Hull's jwt.verify and csrf.verify use constant-time internally
```

### 7. API Consistency (JS vs Lua parity)

| Issue | What to Check | Severity |
|-------|---------------|----------|
| Missing API | Function exists in Lua but not JS (or vice versa) | Medium |
| Different behavior | Same function returns different types or formats | High |
| Naming mismatch | API names don't follow convention (JS: camelCase, Lua: snake_case) | Low |
| Different defaults | Default option values differ between runtimes | Medium |
| Error format | Different error message formats | Low |

### 8. Template Engine Specific

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Code injection in codegen | User data interpolated into generated JS source | Critical |
| Circular inheritance | `{% extends %}` chains without cycle detection | High |
| Unbounded recursion | Deeply nested includes without depth limit | High |
| Cache poisoning | Template cache key collision or manipulation | Medium |
| Filter bypass | Custom filter that returns unescaped HTML | Medium |
| Denial of service | Template that generates unbounded output | Medium |
| Prototype pollution in data | Template data object with `__proto__` key | High |

### 9. QuickJS-Specific Issues

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Missing ES2023 polyfills | Using APIs not supported by QuickJS | Medium |
| BigInt overflow | BigInt operations without bounds | Low |
| ArrayBuffer detach | SharedArrayBuffer not available | Low |
| Module resolution | Dynamic `import()` not available | Medium |
| Generator memory | Unbounded generator/iterator state | Medium |

### 10. Dead Code & Style

| Pattern | Issue | Fix |
|---------|-------|-----|
| Unreachable code after `return`/`throw` | Dead code | Remove |
| Unused `const`/`let` variables | Dead variable | Remove |
| Unused function parameters | Dead parameter | Prefix with `_` |
| Commented-out code blocks | Dead code | Remove |
| Unused `import` bindings | Dead import | Remove |
| Empty `if`/`else`/`catch` blocks | Dead branch | Remove |

## Audit Procedure

When `/js-audit` is invoked:

1. **Locate Files**
   ```
   stdlib/js/hull/*.js                     # All JS stdlib modules
   examples/*/app.js                       # Example apps (reference patterns)
   tests/fixtures/*/app.js                 # Test fixture apps
   ```

2. **Scan for Critical Issues**
   - Search for `eval(`, `Function(`, `new Function(` — sandbox escapes
   - Search for template literal SQL: `` db.query(`...${` `` or `db.query("..." +` — SQL injection
   - Search for `{{{ ` in template strings — raw output of user data
   - Search for `===` comparison of secrets, tokens, hashes
   - Search for hardcoded secret strings
   - Search for `globalThis.` or `globalThis[` — global pollution
   - Search for `__proto__`, `constructor.prototype` — prototype pollution

3. **Review Each Module**
   - Check public API functions for input validation
   - Check error handling (null/undefined checks, try/catch)
   - Check resource cleanup (cache sizes, collection growth)
   - Verify API parity with Lua equivalent

4. **Check Template Engine**
   - Verify codegen never interpolates user data into generated source
   - Verify HTML escaping covers `& < > " '`
   - Verify null-safe dot paths (optional chaining)
   - Verify circular extends detection
   - Verify include depth limit

5. **Generate Report**
   Format as markdown table with findings, severity, file:line, and suggested fix.

## Report Format

```markdown
## JS Audit Report: Hull

**Date:** YYYY-MM-DD
**Files Scanned:** N
**Issues Found:** N (Critical: N, High: N, Medium: N, Low: N)

### Critical Issues

| # | File:Line | Issue | Current Code | Suggested Fix |
|---|-----------|-------|--------------|---------------|
| C1 | stdlib/js/hull/auth.js:42 | SQL injection | `` db.query(`...${id}`) `` | `db.query("...?", [id])` |

### High Issues
...

### Medium Issues
...

### Low Issues
...

### Recommendations
1. ...
```

## Fix Mode (--fix)

When `--fix` is specified:

1. Generate the audit report first
2. For each fixable issue, apply the transformation
3. Rebuild (`make`)
4. Re-run tests (`make test && make e2e-templates`)
5. Report any test failures

**Auto-fixable Issues:**
- SQL string interpolation -> parameterized queries
- `==` -> `===` (strict equality)
- Unused variables -> remove or prefix with `_`
- `s += x` in loops -> `array.push` + `.join("")` pattern
- Missing null checks -> add guard clause
- Commented-out code blocks -> remove

**NOT Auto-fixable (require manual review):**
- Logic errors
- Crypto/auth design flaws
- API parity mismatches (may require Lua changes too)
- Template codegen injection paths
- Prototype pollution vectors
- Resource leak in complex control flow
