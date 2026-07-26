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

local function file_exists(p) return tool.file_exists(p) end

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

return M
