--
-- hull.feature_compose — Shared archive-composition helpers
--
-- Internal helper module used by:
--   stdlib/cli/lua/hull/build.lua  (hull build --with=<feature> + the
--                                   auto-composed runtime archive)
--   stdlib/cli/lua/hull/eject.lua  (bundling the runtime feature into an
--                                   ejected native project)
--
-- Centralizing this avoids drift between the release-build compose path and
-- the eject path: one archive-resolution ladder, one whole-archive/force_load
-- link fragment, one app-runtime detector, one per-app feature-registry
-- codegen. Before this module those pieces were hand-copied per call site
-- (see the self-review that motivated it), which is exactly the divergence
-- that let a --with fix miss the runtime path.
--
-- Loaded in the `hull tool` Lua harness, where the `tool.*` globals exist.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

local json = require("hull.json")

local function file_exists(p) return tool.file_exists(p) end

-- ── App manifest extraction (shared by build + eject) ────────────────
--
-- Runs the app entry to capture its app.manifest({...}). EXECUTING app.lua a
-- second time corrupts the sign flow, so the result is cached per app_dir (each
-- `hull build`/`hull eject` is a fresh process, so this is a per-invocation
-- cache). Both `hull build` and the mandatory-compose plan below read the
-- manifest; centralizing avoids running the app twice and keeps one impl.
--
-- @param app_dir string
-- @return table|nil  The captured manifest, or nil if extraction failed.
local _manifest_cache = {}
function M.extract_manifest(app_dir)
    local cached = _manifest_cache[app_dir]
    if cached ~= nil then return cached.manifest end
    local manifest = nil
    local lua_entry = app_dir .. "/app.lua"
    local js_entry  = app_dir .. "/app.js"
    if file_exists(lua_entry) then
        local chunk = tool.loadfile(lua_entry)
        if chunk then
            local ok, err = pcall(chunk)
            if ok then
                manifest = app.get_manifest()
            else
                tool.stderr("hull: warning: Lua manifest extraction failed: "
                            .. tostring(err) .. "\n")
            end
        end
    elseif file_exists(js_entry) then
        local ok, js_json_or_err = pcall(tool.extract_manifest_js, js_entry)
        if not ok then
            tool.stderr("hull: warning: JS manifest extraction failed: "
                        .. tostring(js_json_or_err) .. "\n")
        elseif js_json_or_err then
            local decoded, decode_err = json.decode(js_json_or_err)
            if decoded then
                manifest = decoded
            else
                tool.stderr("hull: warning: JS manifest JSON decode failed: "
                            .. tostring(decode_err) .. "\n")
            end
        end
    end
    _manifest_cache[app_dir] = { manifest = manifest }
    return manifest
end

-- ── App runtime detection ────────────────────────────────────────────

--- Detect the app's single runtime from its entry file.
--
-- The native base is runtime-less, so a produced app composes exactly one
-- runtime archive; the entry-file extension names it.
--
-- @param app_dir string  Application directory.
-- @return "lua" | "js" | nil
function M.detect_app_rt(app_dir)
    if file_exists(app_dir .. "/app.lua") then return "lua" end
    if file_exists(app_dir .. "/app.js") then return "js" end
    return nil
end

-- ── Whole-archive link fragment ──────────────────────────────────────

--- Link tokens that force-load an ENTIRE archive.
--
-- A runtime / whole_archive feature spreads its strong overrides of the
-- base weak hooks across several objects with no single anchor symbol, so
-- the whole archive must be pulled. macOS ld64 uses `-force_load <lib>`;
-- the GNU ld / lld path uses the `--whole-archive ... --no-whole-archive`
-- bracket.
--
-- @param dest string       Path to the archive.
-- @param is_darwin boolean  Target is a Mach-O / ld64 link.
-- @return string[]  Link tokens (append to the link line in order).
function M.whole_archive_flags(dest, is_darwin)
    if is_darwin then
        return { "-Wl,-force_load," .. dest }
    end
    return { "-Wl,--whole-archive", dest, "-Wl,--no-whole-archive" }
end

-- ── Archive resolution ladder ────────────────────────────────────────

--- Resolve a feature / runtime archive by file name.
--
-- Search order: local build dirs (hull_dir, ./build, ../build) first, then
-- the signed feature cache (~/.hull/feature), which is re-verified against
-- its signed manifest (embedded release pubkey) before use. This closes the
-- install-to-build TOCTOU and fails CLOSED: a missing `platform_verify`
-- binding aborts a cache-sourced lib rather than linking it unverified.
--
-- The embedded-in-hull step differs per caller (the runtime is extracted via
-- tool.extract_feature_runtime, a --with feature is not), so it is NOT part
-- of this ladder — callers try their own embedded step first, then fall
-- here.
--
-- @param libname string     Local archive name, e.g. "libhull_feature-lua.a".
-- @param asset_name string|nil  Cache asset name (arch-qualified), or nil to
--                               skip the cache tier.
-- @param ctx table          { hull_dir = string, plat = string|nil }.
-- @return string, string    (path, "local"|"cache") on success.
-- @return nil, string       (nil, "cache-verify-failed"|"not-found") on failure.
function M.resolve_lib(libname, asset_name, ctx)
    for _, d in ipairs({ ctx.hull_dir or "", "build/", "../build/" }) do
        if file_exists(d .. libname) then return d .. libname, "local" end
    end
    if asset_name and ctx.plat and tool.feature_cache_dir then
        local cache = tool.feature_cache_dir()
        if cache and file_exists(cache .. "/" .. asset_name) then
            if not tool.platform_verify or not tool.platform_verify(cache, asset_name) then
                return nil, "cache-verify-failed"
            end
            return cache .. "/" .. asset_name, "cache"
        end
    end
    return nil, "not-found"
end

-- ── Per-app feature registry codegen ─────────────────────────────────

--- Emit the per-app feature registry C source.
--
-- Every produced app needs STRONG hl_stdlib_feature_entries() +
-- hl_runtime_feature_factories() for its one runtime. The base defaults are
-- weak and, resolved from libhull_platform.a (an archive), win first: the app
-- would get an empty stdlib (require "hull.json" fails) AND no runtime (the
-- base g_factories is empty, so runtimes come only from this hook). This
-- returns a direct object filling both hooks with the app's runtime factory +
-- stdlib array (hl_<rt>_factory / hl_stdlib_<rt>_entries). Because the app
-- references only its own runtime's symbols, the OTHER interpreter (VM +
-- stdlib) is never pulled from the archive and dead-strips -> the slim.
--
-- Self-contained (no libc headers): the structs are only address-taken here,
-- never dereferenced, so layout-compatible local decls suffice (same
-- convention as build.lua's by_hook feature_registry codegen).
--
-- @param rt "lua" | "js"
-- @return string  C source.
function M.gen_app_registry_c(rt)
    local entries = "hl_stdlib_" .. rt .. "_entries"
    local factory = "hl_" .. rt .. "_factory"
    return table.concat({
        "/* Auto-generated feature registry - do not edit. */",
        "typedef __SIZE_TYPE__ size_t;",
        "typedef struct { const char *n; const unsigned char *d; unsigned int l; } HlEntry;",
        "extern const HlEntry " .. entries .. "[];",
        "static const HlEntry *const HL_STDLIB_FEATS[] = { " .. entries .. " };",
        "const HlEntry *const *hl_stdlib_feature_entries(size_t *count) {",
        "    if (count) *count = 1;",
        "    return HL_STDLIB_FEATS;",
        "}",
        "typedef struct HlRuntimeFactory HlRuntimeFactory;",
        "extern const HlRuntimeFactory " .. factory .. ";",
        "static const HlRuntimeFactory *const HL_RT_FEATS[] = { &" .. factory .. " };",
        "const HlRuntimeFactory *const *hl_runtime_feature_factories(size_t *count) {",
        "    if (count) *count = 1;",
        "    return HL_RT_FEATS;",
        "}",
        "",
    }, "\n")
end

--- Resolve the app's runtime archive, trying the embedded-in-hull copy first.
--
-- Shared by build.lua and eject.lua so the "extract embedded -> local build
-- dirs -> signed cache" order (and its fail-closed cache re-verify) lives in
-- one place. The extracted copy lands in `tmpdir/libhull_feature-<rt>.a`.
--
-- @param rt "lua" | "js"
-- @param tmpdir string  Scratch dir the embedded runtime is extracted into.
-- @param ctx table      { hull_dir = string, plat = string|nil }.
-- @return string, string   (path, "embedded"|"local"|"cache") on success.
-- @return nil, string      (nil, "cache-verify-failed"|"not-found") on failure.
function M.resolve_runtime_lib(rt, tmpdir, ctx)
    local libname = "libhull_feature-" .. rt .. ".a"
    -- 1. Embedded in this hull (the distributed native path): extract it.
    if tool.extract_feature_runtime and tool.extract_feature_runtime(tmpdir, rt)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    -- 2/3. Local build dirs, then the signed feature cache.
    local asset = ctx.plat and ("libhull_feature-" .. rt .. "-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the HTTP core feature archive, trying the embedded-in-hull copy first.
--
-- Mirrors resolve_runtime_lib for issue #114: the default distributed native
-- hull embeds libhull_feature-http.a, so a full-flavor app composes it back
-- with no `hull feature install`. Extracted copy lands in
-- `tmpdir/libhull_feature-http.a`.
--
-- @param tmpdir string  Scratch dir the embedded archive is extracted into.
-- @param ctx table      { hull_dir = string, plat = string|nil }.
-- @return string, string   (path, "embedded"|"local"|"cache") on success.
-- @return nil, string      (nil, "cache-verify-failed"|"not-found") on failure.
function M.resolve_http_lib(tmpdir, ctx)
    local libname = "libhull_feature-http.a"
    -- 1. Embedded in this hull (the distributed native path): extract it.
    if tool.extract_feature_http and tool.extract_feature_http(tmpdir)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    -- 2/3. Local build dirs, then the signed feature cache.
    local asset = ctx.plat and ("libhull_feature-http-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the Keel event-loop feature archive (libhull_feature-keel.a),
--- composed onto a Keel-less base on needs_http (docs/keel_feature.md, Phase
--- 4.2b). Embedded-in-hull first (once wired), else the local build dir / cache.
function M.resolve_keel_lib(tmpdir, ctx)
    local libname = "libhull_feature-keel.a"
    if tool.extract_feature_keel and tool.extract_feature_keel(tmpdir)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    local asset = ctx.plat and ("libhull_feature-keel-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the per-runtime web-bindings archive (libhull_feature-http-<rt>.a),
--- trying the embedded-in-hull copy first (issue #114, Phase C).
--
-- The web bindings (routes/dispatch/res:*/ws/sse/http-client/smtp) live in
-- their own per-runtime archive so an HTTP-free app links only the pure runtime.
-- Composed alongside the http core when an app needs HTTP. Extracted copy lands
-- in `tmpdir/libhull_feature-http-<rt>.a`.
--
-- @param rt "lua" | "js"
-- @param tmpdir string  Scratch dir the embedded archive is extracted into.
-- @param ctx table      { hull_dir = string, plat = string|nil }.
-- @return string, string   (path, "embedded"|"local"|"cache") on success.
-- @return nil, string      (nil, "cache-verify-failed"|"not-found") on failure.
function M.resolve_http_rt_lib(rt, tmpdir, ctx)
    local libname = "libhull_feature-http-" .. rt .. ".a"
    -- 1. Embedded in this hull (the distributed native path): extract it.
    if tool.extract_feature_http_rt and tool.extract_feature_http_rt(tmpdir, rt)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    -- 2/3. Local build dirs, then the signed feature cache.
    local asset = ctx.plat and ("libhull_feature-http-" .. rt .. "-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the per-runtime tui bridge (libhull_feature-tui-<rt>.a), embedded
--- first (issue #114, Phase D).
--
-- The tui cap core stays the single installable feature asset; the tiny
-- per-runtime bridge is embedded in hull and composed alongside the cap core
-- for the app's runtime, so `hull feature install tui` still fetches one lib.
--
-- @param rt "lua" | "js"
-- @param tmpdir string  Scratch dir the embedded archive is extracted into.
-- @param ctx table      { hull_dir = string, plat = string|nil }.
-- @return string, string   (path, "embedded"|"local"|"cache") on success.
-- @return nil, string      (nil, "cache-verify-failed"|"not-found") on failure.
function M.resolve_tui_rt_lib(rt, tmpdir, ctx)
    local libname = "libhull_feature-tui-" .. rt .. ".a"
    if tool.extract_feature_tui_rt and tool.extract_feature_tui_rt(tmpdir, rt)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    local asset = ctx.plat and ("libhull_feature-tui-" .. rt .. "-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the WASM core feature archive (libhull_feature-wasm.a), embedded
--- first (docs/wasm_feature.md, Phase 1). Mirrors resolve_http_lib: the native
--- base is compute-less and the default hull embeds the wasm core, so a
--- full-flavor app composes it back with no `hull feature install`.
function M.resolve_wasm_lib(tmpdir, ctx)
    local libname = "libhull_feature-wasm.a"
    if tool.extract_feature_wasm and tool.extract_feature_wasm(tmpdir)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    local asset = ctx.plat and ("libhull_feature-wasm-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the per-runtime compute bridge (libhull_feature-wasm-<rt>.a),
--- embedded first. Holds mod_compute (the compute.* binding); composed alongside
--- the wasm core for the app's runtime, like the http web bindings.
function M.resolve_wasm_rt_lib(rt, tmpdir, ctx)
    local libname = "libhull_feature-wasm-" .. rt .. ".a"
    if tool.extract_feature_wasm_rt and tool.extract_feature_wasm_rt(tmpdir, rt)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    local asset = ctx.plat and ("libhull_feature-wasm-" .. rt .. "-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the SQLite ENGINE archive (libhull_feature-sqlite.a: cap/db_sqlite +
--- vendored sqlite3 + FTS5 + udf cap + sqlite agent), embedded first (Phase D).
--- On an HL_APP_BASE_SQLITELESS distributed hull the engine ships embedded, so a
--- db app auto-composes it with no `hull feature install sqlite`; otherwise this
--- falls back to the local build dir / installed feature cache (the pre-Phase-D
--- path). Mirrors resolve_wasm_lib.
function M.resolve_sqlite_lib(tmpdir, ctx)
    local libname = "libhull_feature-sqlite.a"
    if tool.extract_feature_sqlite and tool.extract_feature_sqlite(tmpdir)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    local asset = ctx.plat and ("libhull_feature-sqlite-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the TLS feature archive (libhull_feature-tls.a: mbedTLS + the crypto
--- mbedTLS backends + tls_client + tls_transport), embedded first (a2). On an
--- HL_APP_BASE_TLSLESS distributed hull it ships embedded, so an HTTPS / net-DB
--- app auto-composes it with no `hull feature install`; otherwise this falls back
--- to the local build dir / installed feature cache. Mirrors resolve_sqlite_lib.
function M.resolve_tls_lib(tmpdir, ctx)
    local libname = "libhull_feature-tls.a"
    if tool.extract_feature_tls and tool.extract_feature_tls(tmpdir)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    local asset = ctx.plat and ("libhull_feature-tls-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the per-runtime SQLite UDF bridge (libhull_feature-sqlite-<rt>.a),
--- embedded first. Holds mod_db_udf (the db.udf bindings); composed for the
--- app's runtime whenever the app uses a udf-capable DB, so the runtime archive
--- itself stays SQLite-free (Phase C.2b).
function M.resolve_sqlite_rt_lib(rt, tmpdir, ctx)
    local libname = "libhull_feature-sqlite-" .. rt .. ".a"
    if tool.extract_feature_sqlite_rt and tool.extract_feature_sqlite_rt(tmpdir, rt)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    local asset = ctx.plat and ("libhull_feature-sqlite-" .. rt .. "-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the image CODEC core archive (libhull_feature-image.a: the codec
--- vtable + stb backend + vendored stb), embedded first (docs/image_feature.md).
--- The native base is image-less and the default hull embeds the image core, so
--- an app declaring hull/image composes it back with no `hull feature install`.
--- Mirrors resolve_wasm_lib.
function M.resolve_image_lib(tmpdir, ctx)
    local libname = "libhull_feature-image.a"
    if tool.extract_feature_image and tool.extract_feature_image(tmpdir)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    local asset = ctx.plat and ("libhull_feature-image-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

--- Resolve the per-runtime image bridge (libhull_feature-image-<rt>.a), embedded
--- first. Holds mod_image (the image.* binding); composed alongside the image
--- core for the app's runtime, like the wasm compute bridge.
function M.resolve_image_rt_lib(rt, tmpdir, ctx)
    local libname = "libhull_feature-image-" .. rt .. ".a"
    if tool.extract_feature_image_rt and tool.extract_feature_image_rt(tmpdir, rt)
       and file_exists(tmpdir .. "/" .. libname) then
        return tmpdir .. "/" .. libname, "embedded"
    end
    local asset = ctx.plat and ("libhull_feature-image-" .. rt .. "-" .. ctx.plat .. ".a") or nil
    return M.resolve_lib(libname, asset, ctx)
end

-- ── The mandatory auto-composed archive plan ─────────────────────────
--
-- The native SLIM base drops runtime + http core + per-runtime web bindings +
-- wasm(+bridge) + sqlite-rt bridge + image(+bridge) + tls + keel, and the
-- produced app composes back exactly what it uses. Both `hull build` (links the
-- archives directly) and `hull eject` (copies them into the ejected project +
-- emits Makefile link flags) need the SAME ordered set. This function is that
-- single source of truth: it computes the needs-gates + nm-probes ONCE and
-- returns the resolved, ORDERED descriptor list. Before this, build.lua composed
-- all 8 while eject.lua composed only runtime+http — the exact drift the module
-- header warns about, one level up (the ladder was shared, the decision to
-- invoke each rung was not).
--
-- @param ctx table Fields:
--   app_dir, app_rt ("lua"|"js"), hull_dir, plat, platform_lib,
--   compute_count (int, # of compute/*.wasm), with (opts.with table|nil),
--   flavor, with_list (array for tool.modules_resolve),
--   on_fail = function(msg)  (caller's cleanup+exit; must not return).
-- @return table { list = { desc, ... }, needs_http = bool }
--   desc = { record_name, lib, path, sets_base_group, is_rt, label }
--     record_name : the composed-asset name (with the .<plat>. infix) for
--                   package.sig attestation + the caller's provenance.
--     lib         : the archive filename (no plat), the copy destination stem.
--     path        : the resolved source archive path.
--     sets_base_group : true iff the archive refs base/Keel symbols (GNU-ld
--                   --start-group). runtime, tls, keel set it.
function M.plan_mandatory(ctx)
    local rt = ctx.app_rt
    local rctx = { hull_dir = ctx.hull_dir or "", plat = ctx.plat }
    local plat_infix = ctx.plat or ""

    -- ── Compute the needs-gates (resolver + build-time signals) ──
    -- needs_http defaults true (fail safe: never under-compose serving); the
    -- resolver flips it false for a pure app.main/compute app.
    local needs_http = true
    -- needs_wasm S2: the app ships compute/*.wasm (catches a WASM-backed db.udf
    -- that declares no module). S1 (a declared WASM cap) ORs in from the resolver.
    local needs_wasm = (ctx.compute_count or 0) > 0
    local needs_sqlite, needs_image, needs_tls = false, false, false
    do
        local mf = M.extract_manifest(ctx.app_dir)
        if mf then
            local r = tool.modules_resolve(mf, ctx.flavor, ctx.with_list)
            if r.ok then
                if r.needs_http ~= nil then needs_http = r.needs_http end
                if r.needs_wasm then needs_wasm = true end
                if r.needs_sqlite then needs_sqlite = true end
                if r.needs_image then needs_image = true end
                if r.needs_tls then needs_tls = true end
            end
        end
    end
    -- A net-DB --with backend (postgres/mysql) links the shared tls_client for
    -- sslmode, so composing it onto a TLS-less base needs the TLS feature too.
    if ctx.with and (ctx.with.postgres or ctx.with.mysql) then needs_tls = true end

    -- Base-presence nm-probes: compose tls/keel ONLY when the base actually
    -- dropped them (HL_TLS_FEATURE=1 / HL_KEEL_FEATURE=1). Default to "present"
    -- (skip) on an unreadable/absent nm, so a stock full base is byte-identical.
    local function base_lacks(sym)
        local out = tool.spawn_read({ "nm", ctx.platform_lib })
        return out and not out:find(sym, 1, true)
    end

    local list = {}
    local function add(spec)
        -- spec: { name (stem), lib, path, sets_base_group, label }
        list[#list + 1] = {
            record_name    = "libhull_feature-" .. spec.name .. "." .. plat_infix .. ".a",
            lib            = spec.lib,
            path           = spec.path,
            sets_base_group = spec.sets_base_group or false,
            label          = spec.label,
        }
    end
    -- Resolve one archive, failing CLOSED via ctx.on_fail with a specific hint.
    -- `make_stem` names the `make feature-<stem>` build-from-source hint.
    local function resolve(kind, make_stem, fn, ...)
        local path, from = fn(...)
        if not path then
            local why = (from == "cache-verify-failed")
                and " (cache re-verify failed)" or " (normally embedded in hull)"
            ctx.on_fail("hull build: the " .. kind .. " feature lib was not found"
                        .. why .. "\nhint: build it from source with `make feature-"
                        .. make_stem .. "`\n")
        end
        return path
    end

    -- 1. Runtime (always; the base is runtime-less). refs base symbols -> group.
    add({ name = rt, lib = "libhull_feature-" .. rt .. ".a", sets_base_group = true,
          label = "runtime '" .. rt .. "'",
          path = resolve("'" .. rt .. "' runtime", rt, M.resolve_runtime_lib, rt, ctx.tmpdir, rctx) })

    -- 2. WASM core + per-runtime compute bridge.
    if needs_wasm then
        add({ name = "wasm", lib = "libhull_feature-wasm.a", label = "WASM feature",
              path = resolve("WASM", "wasm", M.resolve_wasm_lib, ctx.tmpdir, rctx) })
        add({ name = "wasm-" .. rt, lib = "libhull_feature-wasm-" .. rt .. ".a",
              label = "compute bridge",
              path = resolve("compute-bindings", "wasm-" .. rt, M.resolve_wasm_rt_lib, rt, ctx.tmpdir, rctx) })
    end

    -- 3. Per-runtime SQLite UDF bridge (the engine itself is a --with feature).
    if needs_sqlite then
        add({ name = "sqlite-" .. rt, lib = "libhull_feature-sqlite-" .. rt .. ".a",
              label = "SQLite udf bridge",
              path = resolve("SQLite udf-bridge", "sqlite-" .. rt, M.resolve_sqlite_rt_lib, rt, ctx.tmpdir, rctx) })
    end

    -- 4. Image codec core + per-runtime image bridge.
    if needs_image then
        add({ name = "image", lib = "libhull_feature-image.a", label = "image feature",
              path = resolve("image", "image", M.resolve_image_lib, ctx.tmpdir, rctx) })
        add({ name = "image-" .. rt, lib = "libhull_feature-image-" .. rt .. ".a",
              label = "image bridge",
              path = resolve("image-bindings", "image-" .. rt, M.resolve_image_rt_lib, rt, ctx.tmpdir, rctx) })
    end

    -- 5. TLS feature (only when the base is actually TLS-less). refs base+Keel.
    if needs_tls and base_lacks("mbedtls_ssl_handshake") then
        add({ name = "tls", lib = "libhull_feature-tls.a", sets_base_group = true,
              label = "TLS feature",
              path = resolve("TLS", "tls", M.resolve_tls_lib, ctx.tmpdir, rctx) })
    end

    -- 6. HTTP core + (Keel when base-less) + per-runtime web bindings.
    if needs_http then
        add({ name = "http", lib = "libhull_feature-http.a", label = "HTTP feature",
              path = resolve("HTTP", "http", M.resolve_http_lib, ctx.tmpdir, rctx) })
        if base_lacks("hl_async_backend_keel") then
            add({ name = "keel", lib = "libhull_feature-keel.a", sets_base_group = true,
                  label = "Keel event loop",
                  path = resolve("Keel", "keel", M.resolve_keel_lib, ctx.tmpdir, rctx) })
        end
        add({ name = "http-" .. rt, lib = "libhull_feature-http-" .. rt .. ".a",
              label = "web bindings",
              path = resolve("web-bindings", "http-" .. rt, M.resolve_http_rt_lib, rt, ctx.tmpdir, rctx) })
    end

    -- needs_wasm is returned so the caller can print the compute-free skip note;
    -- needs_http likewise for the HTTP-free note (both are grepped by the tui/
    -- wasm feature e2es as over-compose regression guards).
    return { list = list, needs_http = needs_http, needs_wasm = needs_wasm }
end

return M
