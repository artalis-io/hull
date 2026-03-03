---
name: lua-audit
description: Audit Lua stdlib code for security, correctness, and sandbox safety. Use when reviewing or hardening Lua modules.
user-invocable: true
---

# Lua Code Audit Skill

Perform comprehensive security, correctness, and quality audits on Hull Lua stdlib code.

**Target:** $ARGUMENTS (default: all `stdlib/lua/hull/*.lua` files)

## Usage

```
/lua-audit                              # Audit all Lua stdlib modules
/lua-audit stdlib/lua/hull/template.lua # Audit a specific module
/lua-audit --fix                        # Audit and apply fixes
```

## Hull Lua Context

Hull Lua code runs inside a sandboxed Lua 5.4 interpreter:
- **Removed globals:** `io`, `os`, `loadfile`, `dofile`, `load`
- **Available libs:** base, table, string, math, utf8, coroutine
- **Custom `require()`:** resolves only from embedded stdlib registry
- **Memory limit:** 64 MB (custom allocator)
- **C capabilities:** `db`, `crypto`, `time`, `env`, `fs`, `http` — accessed through hull globals, NOT Lua standard libs

## Audit Categories

### 1. Sandbox Safety (Critical)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Sandbox escape | Use of `load()`, `loadstring()`, `dofile()`, `loadfile()` | Critical |
| Unsafe eval | String-to-code conversion outside C bridge `_compile()` | Critical |
| Module smuggling | `require()` of non-hull modules | Critical |
| Global pollution | Writing to `_G` or global scope without `local` | High |
| Metatable abuse | `setmetatable` on shared objects that could affect other modules | High |
| Debug library | Use of `debug.*` (not loaded, but check for attempts) | Critical |
| rawget/rawset bypass | Circumventing metatables to access restricted data | Medium |

**Hull-specific:** The template engine's `_template._compile()` uses `luaL_loadbuffer` via C bridge — this is the ONLY allowed code compilation path. Verify no Lua-level code compilation exists.

### 2. Input Validation & Injection

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| SQL injection | String concatenation in SQL queries | Critical |
| XSS via template | Unescaped user data in template output | High |
| Path traversal | Unsanitized paths passed to `fs.*` | High |
| Header injection | `\r\n` in HTTP header values | High |
| Command injection | User input in `tool.spawn()` arguments | Critical |
| HMAC timing attack | Non-constant-time string comparison of secrets/tokens | High |
| Regex DoS (ReDoS) | Unbounded `string.find`/`string.gmatch` on user input | Medium |

**SQL safety check:**
```lua
-- BAD: string concatenation
db.query("SELECT * FROM users WHERE id = " .. id)

-- GOOD: parameterized
db.query("SELECT * FROM users WHERE id = ?", {id})
```

**Template safety check:**
```lua
-- BAD: raw output of user data
template.render_string("{{{ user_input }}}", data)

-- GOOD: auto-escaped output
template.render_string("{{ user_input }}", data)
```

### 3. Error Handling

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Unchecked nil | Function return used without nil check | High |
| Silent failure | `pcall` that discards error message | Medium |
| Missing error propagation | Error condition not returned to caller | Medium |
| Bare `error()` | Error without context message | Low |
| Unprotected external calls | `db.query()`, `http.get()` without error handling | Medium |

**Patterns to check:**
```lua
-- BAD: unchecked
local result = db.query("SELECT * FROM users WHERE id = ?", {id})
local name = result[1].name  -- crashes if result is empty

-- GOOD: nil-safe
local result = db.query("SELECT * FROM users WHERE id = ?", {id})
if not result or #result == 0 then return nil, "not found" end
local name = result[1].name
```

### 4. Type Safety

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Missing type checks | Function params not validated with `type()` | Medium |
| Nil propagation | Nil values passed through chains without checks | Medium |
| Table/string confusion | `#` operator on potentially nil values | Medium |
| Number coercion | String used where number expected (or vice versa) | Low |
| Boolean truthiness | `0` and `""` are truthy in Lua; `{}` is truthy | Medium |

**Lua truthiness pitfalls:**
```lua
-- BAD: 0 is truthy in Lua!
if count then  -- true even when count == 0

-- GOOD:
if count and count > 0 then

-- BAD: empty table is truthy!
if items then  -- true even when items == {}

-- GOOD:
if items and #items > 0 then
```

### 5. Resource Management

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Unbounded table growth | Tables that grow without limit (caches, logs) | High |
| Missing cache eviction | Caches without TTL or size limit | Medium |
| Closure leaks | Closures capturing large upvalues unnecessarily | Medium |
| String concatenation in loops | `s = s .. chunk` in loops (O(n^2)) | Medium |
| Large intermediate tables | Building tables that could exceed memory limit | Medium |

**Performance patterns:**
```lua
-- BAD: O(n^2) string building
local s = ""
for _, item in ipairs(items) do
    s = s .. item  -- copies entire string each iteration
end

-- GOOD: table.concat
local parts = {}
for _, item in ipairs(items) do
    parts[#parts + 1] = item
end
local s = table.concat(parts)
```

### 6. Crypto & Auth Safety

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Hardcoded secrets | Literal strings used as HMAC/JWT secrets | Critical |
| Weak secrets | Short or predictable secret values | High |
| Timing attacks | `==` comparison on HMAC digests or tokens | High |
| Missing expiry | Tokens/sessions without TTL | Medium |
| Insecure defaults | `Secure` flag missing on cookies, `HttpOnly` not set | Medium |
| Nonce reuse | Same nonce/IV used for multiple encryptions | Critical |

**Constant-time comparison:**
```lua
-- BAD: early-exit comparison
if token == expected then

-- GOOD: use crypto.verify_password or HMAC-then-compare
-- Hull's jwt.verify and csrf.verify use constant-time internally
```

### 7. API Consistency (Lua vs JS parity)

| Issue | What to Check | Severity |
|-------|---------------|----------|
| Missing API | Function exists in JS but not Lua (or vice versa) | Medium |
| Different behavior | Same function returns different types or formats | High |
| Naming mismatch | API names don't follow convention (Lua: snake_case, JS: camelCase) | Low |
| Different defaults | Default option values differ between runtimes | Medium |
| Error format | Different error message formats | Low |

### 8. Template Engine Specific

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Code injection in codegen | User data interpolated into generated Lua source | Critical |
| Circular inheritance | `{% extends %}` chains without cycle detection | High |
| Unbounded recursion | Deeply nested includes without depth limit | High |
| Cache poisoning | Template cache key collision or manipulation | Medium |
| Filter bypass | Custom filter that returns unescaped HTML | Medium |
| Denial of service | Template that generates unbounded output | Medium |

### 9. Dead Code & Style

| Pattern | Issue | Fix |
|---------|-------|-----|
| Unreachable code after `return` | Dead code | Remove |
| Unused local variables | Dead variable | Remove |
| Unused function parameters | Dead parameter | Prefix with `_` |
| Commented-out code blocks | Dead code | Remove |
| `require` of unused module | Dead import | Remove |
| Empty `if`/`else` blocks | Dead branch | Remove |

## Audit Procedure

When `/lua-audit` is invoked:

1. **Locate Files**
   ```
   stdlib/lua/hull/*.lua                   # All Lua stdlib modules
   examples/*/app.lua                      # Example apps (reference patterns)
   tests/fixtures/*/app.lua                # Test fixture apps
   ```

2. **Scan for Critical Issues**
   - Search for `load(`, `loadstring(`, `dofile(`, `loadfile(` — sandbox escapes
   - Search for string concatenation in SQL: `"SELECT.*"..` or `"INSERT.*"..`
   - Search for `{{{ ` in template strings — raw output of user data
   - Search for `==` comparison of secrets, tokens, hashes
   - Search for hardcoded secret strings
   - Search for `_G.` or `_G[` — global pollution
   - Search for missing `local` on function-scoped variables

3. **Review Each Module**
   - Check public API functions for input validation
   - Check error handling (nil returns, pcall usage)
   - Check resource cleanup (cache sizes, table growth)
   - Verify API parity with JS equivalent

4. **Check Template Engine**
   - Verify codegen never interpolates user data into generated source
   - Verify HTML escaping covers `& < > " '`
   - Verify nil-safe dot paths
   - Verify circular extends detection
   - Verify include depth limit

5. **Generate Report**
   Format as markdown table with findings, severity, file:line, and suggested fix.

## Report Format

```markdown
## Lua Audit Report: Hull

**Date:** YYYY-MM-DD
**Files Scanned:** N
**Issues Found:** N (Critical: N, High: N, Medium: N, Low: N)

### Critical Issues

| # | File:Line | Issue | Current Code | Suggested Fix |
|---|-----------|-------|--------------|---------------|
| C1 | stdlib/lua/hull/auth.lua:42 | SQL injection | `db.query("... " .. id)` | `db.query("... ?", {id})` |

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
- SQL string concatenation -> parameterized queries
- Missing `local` declarations -> add `local`
- Unused variables -> remove or prefix with `_`
- `s = s .. x` in loops -> `table.concat` pattern
- Missing nil checks -> add guard clause
- Commented-out code blocks -> remove

**NOT Auto-fixable (require manual review):**
- Logic errors
- Crypto/auth design flaws
- API parity mismatches (may require JS changes too)
- Template codegen injection paths
- Resource leak in complex control flow
