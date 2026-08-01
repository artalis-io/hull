--
-- hull.build — Build a standalone hull application binary
--
-- Usage: hull build [options] [app_dir]
--   --runtime lua|js|both  Runtime to include (default: lua)
--   --sign <key_file>      Sign with Ed25519 private key
--   --compiler tcc|system|<path>  C compiler backend (default: embedded tcc → cc/gcc/clang)
--   --output <path>        Output binary path (default: app_dir/app)
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")
local fcompose = require("hull.feature_compose")

-- ── Argument parsing ─────────────────────────────────────────────────

local function parse_args()
    local opts = {
        runtime = "lua",
        sign = nil,
        cc = nil,         -- resolved from tool.cc (set by C, default cosmocc)
        output = nil,
        app_dir = ".",
        aot = true,       -- AOT compile WASM modules (--no-aot to disable)
        cache = true,     -- Use the AOT artifact cache at
                          -- $HOME/.hull/cache/compute-aot/. --no-cache
                          -- forces a fresh wamrc invocation (still
                          -- writes results back to the cache).
        target = nil,     -- cross-compilation target arch (e.g. "x86_64", "aarch64")
        build_compute = true, -- Auto-rebuild compute/<name>/<name>.c → .wasm
                              -- (--no-build-compute to disable)
        verify_platform = true, -- Cross-check libhull_platform.a SHA-256
                                -- against the embedded signed manifest.
                                -- --no-verify-platform skips it; use for
                                -- dev hulls (no embedded manifest) or
                                -- forks signing with their own platform key.
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--runtime" then
            i = i + 1
            opts.runtime = arg[i]
        elseif a == "--sign" then
            i = i + 1
            opts.sign = arg[i]
        elseif a == "--compiler" then
            i = i + 1
            local comp = arg[i]
            if comp == "tcc" then
                opts.cc = "tcc"
            elseif comp == "system" then
                opts.cc = "system"
            else
                opts.cc = comp
            end
        elseif a == "--output" or a == "-o" then
            i = i + 1
            opts.output = arg[i]
        elseif a == "--no-aot" then
            opts.aot = false
        elseif a == "--no-cache" then
            opts.cache = false
        elseif a == "--no-build-compute" then
            opts.build_compute = false
        elseif a == "--target" then
            i = i + 1
            opts.target = arg[i]
        elseif a == "--no-verify-platform" then
            opts.verify_platform = false
        elseif a == "--flavor" then
            i = i + 1
            opts.flavor = arg[i]
        elseif a:sub(1, 9) == "--flavor=" then
            opts.flavor = a:sub(10)
        elseif a == "--with" then
            i = i + 1
            opts.with = opts.with or {}
            for f in tostring(arg[i]):gmatch("[^,]+") do opts.with[f] = true end
        elseif a:sub(1, 7) == "--with=" then
            opts.with = opts.with or {}
            for f in a:sub(8):gmatch("[^,]+") do opts.with[f] = true end
        elseif a:sub(1, 1) ~= "-" then
            opts.app_dir = a
        end
        i = i + 1
    end

    if not opts.output then
        opts.output = opts.app_dir .. "/app"
    end

    return opts
end

-- ── File utilities ───────────────────────────────────────────────────

local function read_file(path)
    return tool.read_file(path)
end

local function write_file(path, data)
    return tool.write_file(path, data)
end

local function file_exists(path)
    return tool.file_exists(path)
end

-- List .lua files recursively in a directory (using tool.find_files)
local function find_lua_files(dir)
    return tool.find_files(dir, "*.lua")
end

-- List .json data files recursively in a directory (excludes static/ and templates/)
local function find_json_files(dir)
    local all = tool.find_files(dir, "*.json")
    local result = {}
    local static_prefix = dir .. "/static/"
    local tpl_prefix = dir .. "/templates/"
    for _, f in ipairs(all) do
        if f:sub(1, #static_prefix) ~= static_prefix and
           f:sub(1, #tpl_prefix) ~= tpl_prefix then
            result[#result + 1] = f
        end
    end
    return result
end

-- Find wamrc binary. Lookup order is implemented in C by
-- `hl_tools_lookup_path` (see include/hull/tools_install.h):
--   1. $HOME/.hull/tools/wamrc   (canonical install for `hull tools install`)
--   2. dirname(hull_exe)/wamrc   (ejected / portable installs)
--   3. wamrc on $PATH            (system / brew install)
-- Build-tree convention `./build/wamrc` is checked as a dev fallback.
local function find_wamrc()
    local p = tool.find_tool("wamrc")
    if p then return p end
    if file_exists("build/wamrc") then return "./build/wamrc" end
    return nil
end

-- Detect if a WASM binary uses Memory64 (64-bit memory addressing).
-- Checks the memory section (ID 5) limits flags for bit 2 (0x04).
local function is_memory64_wasm(path)
    local data = tool.read_file(path)
    if not data then return false end
    if #data < 8 then return false end
    -- Skip 8-byte WASM header, then scan sections
    local pos = 9  -- 1-indexed
    while pos <= #data do
        local section_id = data:byte(pos)
        pos = pos + 1
        -- Decode LEB128 section size
        local size = 0
        local shift = 0
        repeat
            if pos > #data then return false end
            local b = data:byte(pos)
            pos = pos + 1
            size = size + ((b % 128) * (2 ^ shift))
            shift = shift + 7
        until b < 128
        if section_id == 5 then  -- Memory section
            -- First byte in section is count (LEB128), then limits flags
            if pos > #data then return false end
            local count_b = data:byte(pos)  -- usually 1
            if count_b < 1 then return false end
            -- Skip count LEB128
            repeat
                if pos > #data then return false end
                local b = data:byte(pos)
                pos = pos + 1
            until b < 128
            -- Now at limits flags byte
            if pos > #data then return false end
            local flags = data:byte(pos)
            return (flags % 8) >= 4  -- bit 2 = Memory64
        end
        pos = pos + size
    end
    return false
end

-- Detect target architecture from compiler toolchain
local function detect_target_arch(cc)
    if cc:find("aarch64") then return "aarch64" end
    if cc:find("arm64") then return "aarch64" end
    if cc:find("x86_64") then return "x86_64" end
    local out = tool.spawn_read({cc, "-dumpmachine"})
    if out then
        if out:find("aarch64") or out:find("arm64") then return "aarch64" end
        if out:find("x86_64") or out:find("amd64") then return "x86_64" end
    end
    return nil
end

-- List .js files recursively in a directory (excludes static/, templates/, node_modules/)
local function find_js_files(dir)
    local all = tool.find_files(dir, "*.js")
    local result = {}
    local static_prefix = dir .. "/static/"
    local tpl_prefix = dir .. "/templates/"
    local nm_prefix = dir .. "/node_modules/"
    for _, f in ipairs(all) do
        if f:sub(1, #static_prefix) ~= static_prefix and
           f:sub(1, #tpl_prefix) ~= tpl_prefix and
           f:sub(1, #nm_prefix) ~= nm_prefix then
            result[#result + 1] = f
        end
    end
    return result
end

-- List .html files recursively in a directory
local function find_html_files(dir)
    return tool.find_files(dir, "*.html")
end

-- List all files recursively in a directory. Used for static-asset
-- enumeration where `static/vendor/htmx.min.js` etc. are legitimate
-- vendored assets that must be embedded — pass include_vendor=true
-- so tool.find_files doesn't skip the vendor/ subdir (its default
-- skip is for source-tree walks).
local function find_all_files(dir)
    return tool.find_files(dir, "*", { include_vendor = true })
end

-- List .sql files in a directory (non-recursive, sorted)
local function find_sql_files(dir)
    local files = tool.find_files(dir, "*.sql")
    table.sort(files)
    return files
end

-- ── xxd in Lua ───────────────────────────────────────────────────────

local function xxd_data(varname, data)
    local lines = {}
    lines[#lines + 1] = "static const unsigned char " .. varname .. "[] = {"
    for i = 1, #data, 12 do
        local chunk = {}
        for j = i, math.min(i + 11, #data) do
            chunk[#chunk + 1] = string.format("0x%02x", data:byte(j))
        end
        lines[#lines + 1] = "  " .. table.concat(chunk, ", ") .. ","
    end
    lines[#lines + 1] = "};"
    return table.concat(lines, "\n")
end

-- ── Build steps ──────────────────────────────────────────────────────

-- Escape a string for safe embedding in a C string literal. App file paths
-- come from tool.find_files (raw on-disk names) and may contain ", \, or
-- control bytes; without escaping a crafted filename could break out of the
-- generated app_registry.c string literal. Belt-and-suspenders with the
-- varname sanitizer below (which forces a valid C identifier).
local function c_string_escape(s)
    return (s:gsub('[%z\1-\31"\\]', function(ch)
        if ch == '"' then return '\\"' end
        if ch == '\\' then return '\\\\' end
        return string.format('\\%03o', ch:byte())
    end))
end

local function generate_app_registry(app_dir, files)
    local parts = {}
    local entries = {}

    parts[#parts + 1] = "/* Auto-generated unified app registry by hull build — do not edit */"
    parts[#parts + 1] = ""

    -- Helper to embed a file and add an entry
    local function add_file(path, entry_name, var_prefix)
        local data = read_file(path)
        if not data then
            tool.stderr("hull build: cannot read " .. path .. "\n")
            tool.exit(1)
        end

        local rel = path:sub(#app_dir + 2) -- strip "dir/"
        -- Force a valid C identifier: map every non-alphanumeric byte to "_"
        -- (a filename with a dash/space/quote would otherwise emit invalid C).
        local varname = var_prefix .. rel:gsub("[^%w]", "_")

        parts[#parts + 1] = xxd_data(varname, data)
        parts[#parts + 1] = ""

        entries[#entries + 1] = string.format(
            '    { "%s", %s, sizeof(%s) },', c_string_escape(entry_name), varname, varname)
    end

    -- Lua modules: "./path" (no .lua extension)
    for _, path in ipairs(files.lua or {}) do
        local rel = path:sub(#app_dir + 2)
        add_file(path, "./" .. rel:gsub("%.lua$", ""), "app_")
    end

    -- JS modules: "./path.js" (keep extension)
    for _, path in ipairs(files.js or {}) do
        local rel = path:sub(#app_dir + 2)
        add_file(path, "./" .. rel, "app_js_")
    end

    -- JSON data: "./path.json" (keep extension)
    for _, path in ipairs(files.json or {}) do
        local rel = path:sub(#app_dir + 2)
        add_file(path, "./" .. rel, "app_")
    end

    -- Templates: "templates/path" (relative from app_dir)
    for _, path in ipairs(files.html or {}) do
        local rel = path:sub(#app_dir + 2) -- e.g. "templates/base.html"
        add_file(path, rel, "tpl_")
    end

    -- Static files: "static/path" (relative from app_dir)
    for _, path in ipairs(files.static or {}) do
        local rel = path:sub(#app_dir + 2) -- e.g. "static/style.css"
        add_file(path, rel, "static_")
    end

    -- Migrations: "migrations/path" (relative from app_dir)
    for _, path in ipairs(files.sql or {}) do
        local rel = path:sub(#app_dir + 2) -- e.g. "migrations/001_init.sql"
        add_file(path, rel, "migration_")
    end

    -- Compute modules: "compute/path" (relative from app_dir)
    for _, path in ipairs(files.compute or {}) do
        local rel = path:sub(#app_dir + 2) -- e.g. "compute/score.wasm"
        add_file(path, rel, "compute_")
    end

    -- WGSL shaders: "shaders/name.wgsl" (relative from app_dir)
    for _, path in ipairs(files.shaders or {}) do
        local rel = path:sub(#app_dir + 2) -- e.g. "shaders/score.wgsl"
        add_file(path, rel, "shader_")
    end

    -- AOT-compiled compute modules (generated in tmpdir, explicit entry names).
    -- By the time we reach this loop the AOT loop above has either compiled
    -- via wamrc or populated `item.path` from a warm cache hit; in BOTH cases
    -- the file is supposed to exist. A nil read here means the build pipeline
    -- silently lost an artifact (truncated cache hit, race with cleanup, etc.)
    -- — silently omitting the AOT entry from the embedded registry would
    -- produce a binary that boots fine but quietly falls back to the WASM
    -- interpreter for that module. Better to fail the build loudly.
    for _, item in ipairs(files.compute_aot or {}) do
        local data = read_file(item.path)
        if not data then
            error(string.format(
                "hull build: AOT artifact missing or unreadable: %s (entry %s). "
                .. "This indicates a corrupted cache hit or a tmpdir race. "
                .. "Re-run with --no-cache to bypass the AOT cache, or run "
                .. "`hull cache verify --kind=compute-aot --repair` to clear "
                .. "the corrupt entry.",
                item.path, item.entry_name))
        end
        local varname = "aot_" .. item.entry_name:gsub("[^%w]", "_")
        parts[#parts + 1] = xxd_data(varname, data)
        parts[#parts + 1] = ""
        entries[#entries + 1] = string.format(
            '    { "%s", %s, sizeof(%s) },', c_string_escape(item.entry_name), varname, varname)
    end

    -- Sort entries by name for O(log n) binary search in HlVfs
    table.sort(entries, function(a, b)
        -- Extract entry name from '    { "name", ...' format
        local na = a:match('"([^"]+)"')
        local nb = b:match('"([^"]+)"')
        return (na or "") < (nb or "")
    end)

    parts[#parts + 1] = '#include "entry.h"'
    parts[#parts + 1] = "const HlEntry hl_app_entries[] = {"
    for _, e in ipairs(entries) do
        parts[#parts + 1] = e
    end
    parts[#parts + 1] = "    { 0, 0, 0 }"
    parts[#parts + 1] = "};"

    return table.concat(parts, "\n")
end

-- Execute the app entry to capture its manifest table (or nil). Lua entry
-- runs in this same VM; JS entry runs in a transient HlJS via
-- tool.extract_manifest_js and the JSON result is decoded into the same
-- shape app.get_manifest() returns. Used by sign_app (to persist the
-- resolved module set) and by main (to validate --flavor before building).
-- Memoized per app_dir: running app.lua calls app.manifest(), which is guarded
-- "can only be called once" per tool VM, so a SECOND extraction of the same app
-- fails and returns nil. Several build phases need the manifest (feature
-- inference, --flavor=auto, flavor validation, package.sig), so cache the first
-- result and hand it back on subsequent calls without re-running the entry. The
-- entry (nil-safe) is wrapped so a cached "no manifest" is distinct from
-- "not yet extracted".
local _manifest_cache = {}
local function extract_app_manifest(app_dir)
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
                tool.stderr("hull build: warning: Lua manifest extraction failed: " .. tostring(err) .. "\n")
            end
        end
    elseif file_exists(js_entry) then
        local ok, js_json_or_err = pcall(tool.extract_manifest_js, js_entry)
        if not ok then
            tool.stderr("hull build: warning: JS manifest extraction failed: " .. tostring(js_json_or_err) .. "\n")
        elseif js_json_or_err then
            local decoded, decode_err = json.decode(js_json_or_err)
            if decoded then
                manifest = decoded
            else
                tool.stderr("hull build: warning: JS manifest JSON decode failed: " .. tostring(decode_err) .. "\n")
            end
        end
    end
    _manifest_cache[app_dir] = { manifest = manifest }
    return manifest
end

-- On a signing failure, clean up the build tmpdir and remove the already-linked
-- but unsigned output binary, so a failed `hull build --sign` never leaves an
-- unsigned binary in place (nor a stray tmpdir). tmpdir/output are nil-safe.
local function sign_fail(msg, tmpdir, output)
    tool.stderr(msg)
    if tmpdir then tool.rmdir(tmpdir) end
    if output then tool.remove_file(output) end
    tool.exit(1)
end

-- Composed features (--with) as a plain array, for tool.modules_resolve's 3rd
-- arg: admits the build caps those features supply (e.g. gpu -> hull/gpu) so
-- the base build-tool resolver accepts modules the feature fills in the app.
local function with_feature_list(opts)
    local t = {}
    if opts.with then for f in pairs(opts.with) do t[#t + 1] = f end end
    return t
end

local function sign_app(app_dir, key_file, sign_ctx, files, tmpdir, output)
    local key_data = read_file(key_file)
    if not key_data then
        sign_fail("hull build: cannot read key file: " .. key_file .. "\n", tmpdir, output)
    end
    local sk_hex = key_data:match("^(%x+)")
    if not sk_hex or #sk_hex ~= 128 then
        sign_fail("hull build: invalid key file format\n", tmpdir, output)
    end

    -- Derive public key
    local pk_file = key_file:gsub("%.key$", ".pub")
    local pk_data = read_file(pk_file)
    local pk_hex = pk_data and pk_data:match("^(%x+)") or ""

    -- Compute file hashes (all embedded file types)
    local file_hashes = {}
    local all_lists = {
        files.js or {},
        files.json or {},
        files.lua or {},
        files.migrations or {},
        files.static or {},
        files.templates or {},
    }
    for _, list in ipairs(all_lists) do
        for _, path in ipairs(list) do
            local data = read_file(path)
            local rel = path:sub(#app_dir + 2)
            file_hashes[rel] = crypto.sha256(data)
        end
    end

    -- Capture the app's manifest (shared with main's --flavor validation).
    local manifest = extract_app_manifest(app_dir)

    -- Resolve the manifest's modules block against the canonical registry.
    -- The result is the full set of admitted modules (declared + intrinsic
    -- core), persisted into package.sig as `modules_resolved`. Auditors can
    -- then read the bundle and see exactly which first-party module surface
    -- the app shipped with, without executing any code. The signature covers
    -- this field, so tampering invalidates the package.
    local modules_resolved = nil
    if manifest then
        local r = tool.modules_resolve(manifest, sign_ctx.flavor, sign_ctx.features)
        if r.ok then
            modules_resolved = r.modules
        else
            tool.stderr("hull build: warning: module resolver failed: " .. tostring(r.error) .. "\n")
        end
    end

    -- Read platform.sig (required for --sign)
    local platform = nil
    if sign_ctx.platform_sig_path then
        local psig_data = read_file(sign_ctx.platform_sig_path)
        if psig_data then
            platform = json.decode(psig_data)
        end
    end
    if not platform then
        tool.stderr("hull build: cannot read platform.sig (required for --sign)\n")
        tool.stderr("hint: run `hull sign-platform <key>` first\n")
        tool.exit(1)
    end

    -- ── v0.1.3: embed the gethull-signed platform manifest blob ──
    -- The existing `platform` table from platform.sig is the
    -- developer's local-signing layer (kept for backward compat +
    -- fork-deployable setups). The new `gethull` sub-table is the
    -- release-side signed manifest from the hull binary that's doing
    -- the build. Runtime --verify-sig (C4) will validate the
    -- gethull.signature against HL_PLATFORM_PUBKEY_HEX.
    --
    -- Absent when --no-verify-platform was passed AND this hull has
    -- no embedded blob (purely local dev). Empty arch_hashes when
    -- --no-verify-platform was passed but a blob exists (the manifest
    -- + signature get inherited from the building hull but no per-arch
    -- cross-check was performed).
    if sign_ctx.platform_sig_blob then
        platform.gethull = {
            manifest  = sign_ctx.platform_sig_blob.manifest,
            signature = sign_ctx.platform_sig_blob.signature,
            arch_hashes = sign_ctx.platform_arch_hashes or {},
        }

        -- Composed-feature attestation (issue #114, docs/composed_feature_signing.md).
        -- Every archive composed into this app above is recorded here so runtime
        -- --verify-sig can prove each one is a genuine gethull.dev artefact, not just
        -- the base platform lib. Two trust domains:
        --   platform_domain: archives that ship INSIDE hull (runtime / http core /
        --     web bindings / tui bridge). Re-attested by the SAME platform-key
        --     manifest that covers the base lib, so the runtime cross-checks each
        --     recorded hash against gethull.manifest under HL_PLATFORM_PUBKEY_HEX.
        --   release_domain: externally-installed --with/--flavor libs, whose trust
        --     anchor is the release-key hull.sha256 (already re-verified at compose).
        --     Recorded for provenance; covered by the developer app-signature.
        local plat_assets, rel_assets = {}, {}
        for _, a in ipairs(sign_ctx.composed_assets or {}) do
            local entry = { name = a.name, sha256 = a.sha256 }
            if a.domain == "release" then
                rel_assets[#rel_assets + 1] = entry
            else
                plat_assets[#plat_assets + 1] = entry
            end
        end
        if #plat_assets > 0 or #rel_assets > 0 then
            platform.gethull.composed = {
                platform_domain = {
                    manifest  = sign_ctx.platform_sig_blob.manifest,
                    signature = sign_ctx.platform_sig_blob.signature,
                    assets    = plat_assets,
                },
                release_domain = { assets = rel_assets },
            }
        end
    end

    -- Capture compiler version
    local cc_version = tool.compiler and tool.compiler.version() or nil

    -- Build the signed payload (canonical JSON key order). The `manifest`
    -- field captures the app's declarations as written; `modules_resolved`
    -- captures the closure (declared + intrinsics) so the signed surface
    -- is unambiguous even across hull-version upgrades.
    local payload_table = {
        binary_hash = sign_ctx.binary_hash,
        build = {
            cc = tool.compiler and tool.compiler.name() or (sign_ctx.cc or "unknown"),
            cc_version = cc_version,
            flags = "-std=c11 -O2",
        },
        files = file_hashes,
        manifest = manifest,
        modules_resolved = modules_resolved,
        platform = platform,
        trampoline_hash = sign_ctx.trampoline_hash,
    }
    local payload = json.encode(payload_table)
    local sig_hex = crypto.ed25519_sign(payload, sk_hex)

    -- Write package.sig
    local sig_table = {
        binary_hash = sign_ctx.binary_hash,
        build = payload_table.build,
        files = file_hashes,
        manifest = manifest,
        modules_resolved = modules_resolved,
        platform = platform,
        trampoline_hash = sign_ctx.trampoline_hash,
        signature = sig_hex,
        public_key = pk_hex,
    }

    local pkg_sig = json.encode(sig_table)
    write_file(app_dir .. "/package.sig", pkg_sig .. "\n")
    print("wrote " .. app_dir .. "/package.sig")
end

local function main()
    local opts = parse_args()

    -- Find app source files
    local lua_files = find_lua_files(opts.app_dir)
    local js_files = find_js_files(opts.app_dir)
    local json_files = find_json_files(opts.app_dir)

    if #lua_files == 0 and #js_files == 0 then
        tool.stderr("hull build: no .lua or .js files found in " .. opts.app_dir .. "\n")
        tool.exit(1)
    end

    if #lua_files > 0 then
        print("hull build: " .. #lua_files .. " Lua file(s) from " .. opts.app_dir)
    end
    if #js_files > 0 then
        print("hull build: " .. #js_files .. " JS file(s) from " .. opts.app_dir)
    end
    if #json_files > 0 then
        print("hull build: " .. #json_files .. " JSON data file(s) from " .. opts.app_dir)
    end

    -- Create temp directory
    local tmpdir = tool.tmpdir()

    -- Write entry.h (shared type definition for all registry files)
    write_file(tmpdir .. "/entry.h", [[
#ifndef HL_ENTRY_H
#define HL_ENTRY_H
typedef struct {
    const char *name;
    const unsigned char *data;
    unsigned int len;
} HlEntry;
#endif
]])

    -- Discover all file types
    local templates_dir = opts.app_dir .. "/templates"
    local html_files = {}
    if file_exists(templates_dir) then
        html_files = find_html_files(templates_dir)
    end
    if #html_files > 0 then
        print("hull build: " .. #html_files .. " template(s) from " .. templates_dir)
    end

    local static_dir = opts.app_dir .. "/static"
    local static_files = {}
    if file_exists(static_dir) then
        static_files = find_all_files(static_dir)
    end
    if #static_files > 0 then
        print("hull build: " .. #static_files .. " static file(s) from " .. static_dir)
    end

    local migrations_dir = opts.app_dir .. "/migrations"
    local migration_files = {}
    if file_exists(migrations_dir) then
        migration_files = find_sql_files(migrations_dir)
    end
    if #migration_files > 0 then
        print("hull build: " .. #migration_files .. " migration(s) from " .. migrations_dir)
    end

    local compute_dir = opts.app_dir .. "/compute"

    -- Auto-rebuild compute source → .wasm before discovery (so the new
    -- artifacts get picked up in this same build pass).
    --
    -- We invoke the same logic that `hull compute build` uses, gated on
    -- staleness so unchanged sources don't trigger clang. If clang isn't
    -- installed and there's nothing stale, we emit no output at all. If
    -- there IS stale work and clang is missing, we treat that as a build
    -- error — the developer asked for an integrated build but we cannot
    -- produce the artifact they expect.
    if opts.build_compute then
        local cbuild = require("hull.compute_build")
        local r = cbuild.build_all(opts.app_dir, "stale")
        if #r.errors > 0 then
            if r.no_clang then
                tool.stderr("hull build: cannot rebuild compute module(s) — clang not found.\n")
                tool.stderr("  Missing/stale: ")
                local names = {}
                for _, e in ipairs(r.errors) do names[#names + 1] = e.name end
                tool.stderr(table.concat(names, ", ") .. "\n")
                tool.stderr("  Either install clang with wasm32 target support, run\n")
                tool.stderr("    hull compute build  (on a machine with clang)\n")
                tool.stderr("  and commit the resulting .wasm files, or pass --no-build-compute\n")
                tool.stderr("  to embed the existing .wasm files as-is.\n")
            else
                for _, e in ipairs(r.errors) do
                    tool.stderr("hull build: compute/" .. e.name .. ": " .. e.err .. "\n")
                end
            end
            tool.rmdir(tmpdir)
            tool.exit(1)
        end
        if #r.built > 0 then
            print("hull build: compiled " .. #r.built .. " compute source(s): " ..
                  table.concat(r.built, ", "))
        end
    end

    local compute_files = {}
    if file_exists(compute_dir) then
        local wasm = tool.find_files(compute_dir, "*.wasm")
        for _, f in ipairs(wasm) do
            compute_files[#compute_files + 1] = f
        end
        local aot = tool.find_files(compute_dir, "*.aot.*")
        for _, f in ipairs(aot) do
            compute_files[#compute_files + 1] = f
        end
    end
    if #compute_files > 0 then
        print("hull build: " .. #compute_files .. " compute module(s) from " .. compute_dir)
    end

    local shaders_dir = opts.app_dir .. "/shaders"
    local shader_files = {}
    if file_exists(shaders_dir) then
        shader_files = tool.find_files(shaders_dir, "*.wgsl")
    end
    if #shader_files > 0 then
        print("hull build: " .. #shader_files .. " shader(s) from " .. shaders_dir)
    end

    -- Resolve CC early (needed for AOT arch detection below).
    -- Priority:
    --   1. Explicit --compiler flag (opts.cc).
    --   2. tool.compiler.name() — the new compiler-vtable API,
    --      reports what hull is actually configured to use
    --      (system cc / tcc / cosmocc, matches the running hull's
    --      build target).
    --   3. tool.cc — legacy compile-time HL_DEFAULT_CC (always
    --      "cosmocc"); kept as a last-resort fallback because some
    --      older code paths still set it explicitly.
    -- Bare "cosmocc" fallback removed: it made native (non-cosmo)
    -- hulls misidentify as cosmo and look up cosmo .a hashes in
    -- the platform-sig cross-check.
    local cc = opts.cc
        or (tool.compiler and tool.compiler.name())
        or tool.cc
        or "cc"
    -- is_cosmo detection: must reflect what's EMBEDDED in this hull
    -- binary (cosmo embeds both x86_64+aarch64 .a's, native embeds
    -- one .a), not what compiler the USER has available. The CC
    -- variable above can mislead — a cosmo hull running on a Linux
    -- box may resolve `tool.compiler.name()` to "cc" because tcc
    -- isn't viable on cosmo. The platform_archs table is the
    -- authoritative signal: multiple "cosmo-*" entries → cosmo
    -- build; single non-cosmo entry → native build; nil → unsigned
    -- local build (no embedded platform).
    local is_cosmo = false
    if tool.platform_archs then
        for _, arch in ipairs(tool.platform_archs() or {}) do
            if arch:find("^cosmo-") then is_cosmo = true; break end
        end
    end
    -- Last-resort fallback for hull builds where platform_archs
    -- isn't available (very old hulls, mocked test harnesses):
    -- fall back to the compiler name. The cosmocc CC name is a
    -- reliable cosmo signal — a real cosmo build invokes cosmocc.
    if not is_cosmo then
        is_cosmo = cc:find("cosmocc") ~= nil
    end

    -- Auto-compose the tui feature when the app declares hull/tui. TUI migrated
    -- from a base-builtin capability to a composable feature
    -- (docs/features_and_flavors.md): the base platform lib is TUI-free so lean
    -- apps stay lean, but an app that declares hull/tui must still build with a
    -- STOCK hull without the user hand-passing --with=tui. So infer it here
    -- (before flavor resolution, so --flavor=auto sees the composed cap). gpu /
    -- duckdb are NOT inferred: they were always opt-in with no built-in past to
    -- preserve. Skip when the base already provides tui as a build cap -- a
    -- monolithic HL_ENABLE_TUI build, or cosmo (where features are native-only
    -- and TUI is compiled in) -- since composing an archive would be wrong or
    -- unavailable there. An explicit --with=tui is a harmless superset.
    --
    -- Only a REQUIRED hull/tui is inferred. An optional "hull/tui@1?" opts into
    -- graceful fallback (require returns nil when absent), so forcing a compose
    -- -- which hard-errors if the feature isn't installed -- would break that
    -- contract; it's left to the resolver's optional-absent path instead.
    if not (opts.with and opts.with.tui) then
        local base_caps = (tool.build_caps and tool.build_caps()) or {}
        if not base_caps.tui and not is_cosmo then
            local m = extract_app_manifest(opts.app_dir)
            local declares_tui = false
            if m and type(m.modules) == "table" then
                for _, v in pairs(m.modules) do
                    -- Value is the canonical spec in both the array
                    -- (`{"hull/tui@1"}`) and keyed (`{tui="hull/tui@1"}`) forms.
                    -- A trailing "?" marks it optional; strip "@major" and "?"
                    -- for the name compare.
                    if type(v) == "string" then
                        local optional = v:match("%?%s*$") ~= nil
                        local name = v:gsub("@.*$", ""):gsub("%?%s*$", "")
                        if name == "hull/tui" and not optional then
                            declares_tui = true
                            break
                        end
                    end
                end
            end
            if declares_tui then
                opts.with = opts.with or {}
                opts.with.tui = true
                -- Record that this feature was inferred (not hand-passed via
                -- --with) so a missing archive later reports a manifest-framed
                -- "not installed" error instead of a bare --with resolution miss.
                opts.with_inferred = opts.with_inferred or {}
                opts.with_inferred.tui = true
                print("hull build: composing feature 'tui' (app declares hull/tui)")
            end
        end
    end

    -- --flavor=auto: infer the minimal flavor from the app's declared modules
    -- (resolve with full caps, then pick the smallest flavor that still
    -- satisfies them). Resolves to a concrete flavor name the block below
    -- handles. Falls back to "full" if the manifest can't be read.
    if opts.flavor == "auto" then
        local picked = "full"
        local manifest = extract_app_manifest(opts.app_dir)
        if manifest then
            local r = tool.modules_resolve(manifest, nil, with_feature_list(opts))
            if r.ok and r.auto and r.auto.name then
                picked = r.auto.name
            end
        end
        print("hull build: --flavor=auto selected '" .. picked .. "'")
        opts.flavor = picked
    end

    -- ── Build flavor (--flavor) ──
    -- Validate the requested flavor and (for non-default flavors) check the
    -- app's manifest against the TARGET flavor's caps, aborting if a declared
    -- module needs a subsystem the flavor drops. MVP: the flavor's
    -- libhull_platform-<flavor>.a must be built locally; signed fetch is a
    -- follow-on phase. See docs/build_flavors.md.
    local flavor_asset = nil
    if opts.flavor and opts.flavor ~= "full" then
        local fr = tool.build_flavor(opts.flavor)
        if not fr.ok then
            tool.stderr("hull build: " .. tostring(fr.error) .. "\n")
            tool.rmdir(tmpdir)
            tool.exit(1)
        end
        -- A PRESET flavor (empty asset, e.g. pure-compute) builds on the default
        -- composable base and only enforces its cleared caps below; the compose
        -- steps naturally add nothing for an app with no HTTP/TLS. A pre-built
        -- flavor lib (non-empty asset) overrides the base via flavor_asset.
        if fr.asset and fr.asset ~= "" then flavor_asset = fr.asset end
        local manifest = extract_app_manifest(opts.app_dir)
        if manifest then
            local r = tool.modules_resolve(manifest, opts.flavor, with_feature_list(opts))
            if not r.ok then
                tool.stderr("hull build: --flavor=" .. opts.flavor .. ": "
                            .. tostring(r.error) .. "\n")
                tool.rmdir(tmpdir)
                tool.exit(1)
            end
        end
    end

    -- Guard: ensure compiler vtable is available
    if not tool.compiler then
        tool.stderr("hull build: no C compiler available\n")
        tool.stderr("hint: install gcc or clang, or rebuild hull with HL_ENABLE_TCC=1\n")
        tool.rmdir(tmpdir)
        tool.exit(1)
    end

    -- AOT compile WASM modules if wamrc is available
    local compute_aot = {} -- {path=..., entry_name=...} for generated AOT files
    local wasm_only = {}
    for _, f in ipairs(compute_files) do
        if f:match("%.wasm$") then wasm_only[#wasm_only + 1] = f end
    end

    if opts.aot and #wasm_only > 0 then
        local wamrc = find_wamrc()
        if wamrc then
            local targets = {}
            if opts.target then
                targets = { opts.target }
            elseif is_cosmo then
                targets = { "x86_64", "aarch64" }
            else
                local arch = detect_target_arch(cc)
                if arch then targets = { arch } end
            end

            if #targets > 0 then
                local aot_cache = require("hull.aot_cache")
                local wamrc_id = aot_cache.wamrc_version(wamrc)
                -- Disambiguate the two reasons the cache might be off:
                -- the user passed --no-cache (opts.cache == false) vs
                -- an env var (HULL_NO_CACHE / HULL_NO_AOT_CACHE). A silent
                -- flag collapse hides "you set HULL_NO_AOT_CACHE=1 in your
                -- shell three months ago and forgot" from CI users
                -- debugging slow builds.
                local cache_enabled = opts.cache and aot_cache.is_enabled()
                if opts.cache and not aot_cache.is_enabled() then
                    print("hull build: AOT cache disabled via " ..
                          "HULL_NO_CACHE / HULL_NO_AOT_CACHE")
                end
                local cache_hits = 0
                local cache_writes = 0

                for _, wasm_path in ipairs(wasm_only) do
                    local rel = wasm_path:sub(#opts.app_dir + 2) -- e.g. "compute/score.wasm"
                    for _, arch in ipairs(targets) do
                        local aot_name = rel:gsub("%.wasm$", ".aot." .. arch) -- "compute/score.aot.x86_64"
                        local aot_path = tmpdir .. "/" .. aot_name
                        tool.mkdir(tmpdir .. "/compute")

                        local mem64 = is_memory64_wasm(wasm_path)

                        -- Cache lookup. Key folds in wasm content,
                        -- arch, memory64 flag, and wamrc identity —
                        -- anything that can change the AOT bytes.
                        local key, hit
                        if cache_enabled then
                            key = aot_cache.key(wasm_path, arch, mem64, wamrc_id)
                            if key then
                                hit = aot_cache.lookup(key, aot_path)
                            end
                        end

                        local ok
                        if hit then
                            cache_hits = cache_hits + 1
                            print("hull build: AOT " .. rel ..
                                  " -> " .. arch ..
                                  (mem64 and " (memory64)" or "") ..
                                  " [cache hit]")
                            ok = true
                        else
                            local wamrc_args = {wamrc, "--target=" .. arch,
                                                "-o", aot_path, wasm_path}
                            if mem64 then
                                table.insert(wamrc_args, 3, "--enable-memory64")
                            end
                            print("hull build: AOT " .. rel ..
                                  " -> " .. arch ..
                                  (mem64 and " (memory64)" or ""))
                            ok = tool.spawn(wamrc_args)
                            if ok and cache_enabled and key then
                                if aot_cache.store(key, aot_path) then
                                    cache_writes = cache_writes + 1
                                end
                            end
                        end

                        if ok then
                            compute_aot[#compute_aot + 1] = {
                                path = aot_path,
                                entry_name = aot_name,
                            }
                        else
                            print("hull build: warning: AOT failed for " .. rel ..
                                  " (target " .. arch .. ")")
                        end
                    end
                end
                if #compute_aot > 0 then
                    print("hull build: " .. #compute_aot ..
                          " AOT module(s) compiled" ..
                          (cache_hits > 0
                              and (" (" .. cache_hits .. " from cache)")
                              or "") ..
                          (cache_writes > 0
                              and (", " .. cache_writes .. " written to cache")
                              or ""))
                end
            else
                print("hull build: warning: cannot detect target arch, skipping AOT")
            end
        else
            print("hull build: wamrc not found, skipping AOT (install with: make wamrc)")
        end
    end

    -- Generate unified app_registry.c
    local registry_c = generate_app_registry(opts.app_dir, {
        lua         = lua_files,
        js          = js_files,
        json        = json_files,
        html        = html_files,
        static      = static_files,
        sql         = migration_files,
        compute     = compute_files,
        compute_aot = compute_aot,
        shaders     = shader_files,
    })
    write_file(tmpdir .. "/app_registry.c", registry_c)

    -- Generate app_main.c. hl_app_run is the slim app-runner entry (hull_serve
    -- only); it pulls none of the hull dev CLI (dispatch/subcommands/agent), so a
    -- produced single-runtime app stays slim.
    local app_main = [[
extern int hl_app_run(int argc, char **argv);
int main(int argc, char **argv) { return hl_app_run(argc, argv); }
]]
    write_file(tmpdir .. "/app_main.c", app_main)

    -- Extract platform library (if embedded)
    local platform_extracted = false
    local platform_lib = tmpdir .. "/libhull_platform.a"

    -- --flavor: link a locally-built non-default platform lib instead of the
    -- embedded (full) one. The flavor lib isn't covered by the signed
    -- platform manifest yet, so the platform-sig cross-check is skipped (MVP;
    -- signed fetch is a follow-on phase).
    if flavor_asset then
        local hull_dir = __hull_exe and (__hull_exe:match("(.*/)") or "") or ""
        local dirs = { hull_dir, "build/", "../build/", "" }
        if is_cosmo then
            -- Dual-arch: <asset>.x86_64-cosmo.a + <asset>.aarch64-cosmo.a,
            -- laid out the way cosmocc's apelink expects (.aarch64/ counterpart).
            -- Search local build dirs, then the ~/.hull/platform cache where
            -- `hull flavor install <flavor>` stores the fetched+verified pair
            -- (as <asset>.x86_64-cosmo.a / <asset>.aarch64-cosmo.a).
            local search = { hull_dir, "build/", "../build/", "" }
            local cache = tool.platform_cache_dir and tool.platform_cache_dir()
            if cache then search[#search + 1] = cache .. "/" end
            local x86, arm, from_cache
            for _, d in ipairs(search) do
                local a = d .. flavor_asset .. ".x86_64-cosmo.a"
                local b = d .. flavor_asset .. ".aarch64-cosmo.a"
                if file_exists(a) and file_exists(b) then
                    x86, arm = a, b
                    from_cache = (cache ~= nil and d == cache .. "/")
                    break
                end
            end
            if not (x86 and arm) then
                tool.stderr("hull build: --flavor=" .. opts.flavor .. ": "
                            .. flavor_asset .. ".{x86_64,aarch64}-cosmo.a not found"
                            .. " (locally or in ~/.hull/platform)\n")
                tool.stderr("hint: `hull flavor install " .. opts.flavor
                            .. "`, or build from source: `make platform-cosmo-"
                            .. opts.flavor .. "`\n")
                tool.rmdir(tmpdir)
                tool.exit(1)
            end
            -- A lib pulled from ~/.hull/platform is re-verified against its
            -- signed manifest (embedded release pubkey) before linking; a
            -- lib the developer built locally is trusted as-is.
            if from_cache and tool.platform_verify then
                if not (tool.platform_verify(cache, flavor_asset .. ".x86_64-cosmo.a")
                    and tool.platform_verify(cache, flavor_asset .. ".aarch64-cosmo.a")) then
                    tool.stderr("hull build: --flavor=" .. opts.flavor
                        .. ": cached cosmo platform lib failed re-verification; "
                        .. "run `hull flavor install " .. opts.flavor .. "` again\n")
                    tool.rmdir(tmpdir)
                    tool.exit(1)
                end
            end
            tool.copy(x86, platform_lib)
            tool.mkdir(tmpdir .. "/.aarch64")
            tool.copy(arm, tmpdir .. "/.aarch64/libhull_platform.a")
        else
            local cand = {}
            for _, d in ipairs(dirs) do cand[#cand + 1] = d .. flavor_asset .. ".a" end
            -- Cached lib from `hull flavor install <flavor>`: stored
            -- platform-qualified (<asset>-<platform>.a) in ~/.hull/platform.
            local cache = tool.platform_cache_dir and tool.platform_cache_dir()
            local cache_asset, cache_path
            if cache and tool.platform_name then
                cache_asset = flavor_asset .. "-" .. tool.platform_name() .. ".a"
                cache_path = cache .. "/" .. cache_asset
                cand[#cand + 1] = cache_path
            end
            local found = nil
            for _, p in ipairs(cand) do
                if file_exists(p) then found = p; break end
            end
            if not found then
                tool.stderr("hull build: --flavor=" .. opts.flavor
                            .. ": platform lib not found (locally or in ~/.hull/platform)\n")
                tool.stderr("hint: `hull flavor install " .. opts.flavor
                            .. "`, or build from source: `make platform-"
                            .. opts.flavor .. "`\n")
                tool.rmdir(tmpdir)
                tool.exit(1)
            end
            -- Re-verify a cache-sourced lib against its signed manifest before
            -- linking (locally-built libs are trusted as-is).
            if cache_path and found == cache_path and tool.platform_verify then
                if not tool.platform_verify(cache, cache_asset) then
                    tool.stderr("hull build: --flavor=" .. opts.flavor
                        .. ": cached platform lib failed re-verification; "
                        .. "run `hull flavor install " .. opts.flavor .. "` again\n")
                    tool.rmdir(tmpdir)
                    tool.exit(1)
                end
            end
            tool.copy(found, platform_lib)
        end
        platform_extracted = true
        opts.verify_platform = false
    end

    -- Try to find platform library in known locations
    -- 1. Check if build_assets has it embedded (multi-arch cosmo)
    if is_cosmo and tool.platform_archs then
        local archs = tool.platform_archs()
        if archs then
            local ok = tool.extract_platform_cosmo(tmpdir)
            if ok then
                platform_extracted = true
            end
        end
    end

    -- 1b. Single-arch embedded extraction
    if not platform_extracted and tool.extract_platform then
        local ok = tool.extract_platform(tmpdir)
        if ok and file_exists(platform_lib) then
            platform_extracted = true
        end
    end

    -- 2. Check build/ directory (development mode)
    local platform_dir = nil
    if not platform_extracted then
        -- Derive hull binary directory from __hull_exe global
        local hull_dir = ""
        if __hull_exe then
            hull_dir = __hull_exe:match("(.*/)" ) or ""
        end
        local dev_paths = {
            hull_dir,
            "build/",
            "../build/",
        }

        if is_cosmo then
            -- Look for multi-arch cosmo archives
            for _, d in ipairs(dev_paths) do
                local x86 = d .. "libhull_platform.x86_64-cosmo.a"
                local arm = d .. "libhull_platform.aarch64-cosmo.a"
                if file_exists(x86) and file_exists(arm) then
                    tool.copy(x86, tmpdir .. "/libhull_platform.a")
                    tool.mkdir(tmpdir .. "/.aarch64")
                    tool.copy(arm, tmpdir .. "/.aarch64/libhull_platform.a")
                    platform_dir = d
                    platform_extracted = true
                    break
                end
            end
            if not platform_extracted then
                -- Fallback: try single-arch archive (non-fat build)
                for _, d in ipairs(dev_paths) do
                    if file_exists(d .. "libhull_platform.a") then
                        tool.copy(d .. "libhull_platform.a", platform_lib)
                        platform_dir = d
                        platform_extracted = true
                        break
                    end
                end
            end
        else
            -- Single-arch fallback (unchanged)
            for _, d in ipairs(dev_paths) do
                if file_exists(d .. "libhull_platform.a") then
                    tool.copy(d .. "libhull_platform.a", platform_lib)
                    platform_dir = d
                    platform_extracted = true
                    break
                end
            end
        end
    end

    if not platform_extracted then
        if is_cosmo then
            tool.stderr("hull build: cannot find platform archives\n")
            tool.stderr("hint: run `make platform-cosmo` first\n")
        else
            tool.stderr("hull build: cannot find libhull_platform.a\n")
            tool.stderr("hint: run `make platform` first, or use an embedded hull build\n")
        end
        tool.rmdir(tmpdir)
        tool.exit(1)
    end

    -- ── Platform-sig cross-check ──
    -- Hash the libhull_platform.a we're about to embed and compare
    -- against the gethull-signed manifest baked into this hull binary.
    -- A mismatch means either:
    --   (a) this hull was built locally (no embedded manifest), OR
    --   (b) the .a was modified between hull install and now
    -- (a) is the dev workflow; (b) is what the platform-sig chain is
    -- designed to catch.
    --
    -- The signed manifest blob, signature, and the cross-checked
    -- per-arch hashes get written into package.sig.platform later in
    -- sign_app() for runtime verify (C4 will enforce them).
    local platform_sig_blob = nil    -- {manifest, signature} or nil
    local platform_arch_hashes = nil -- {arch: hex, ...} the cross-checked entries
    if opts.verify_platform then
        platform_sig_blob = tool.platform_sig_get()
        if not platform_sig_blob then
            tool.stderr(
                "hull build: this hull has no embedded platform manifest\n" ..
                "       (built locally or from a release that predates platform-sig).\n" ..
                "       Use --no-verify-platform to build anyway; runtime --verify-sig\n" ..
                "       will reject the resulting app unless it too uses --no-verify-platform.\n")
            tool.rmdir(tmpdir)
            tool.exit(1)
        end

        -- Build the list of arches whose .a hashes we cross-check.
        -- Native: just the running arch. Cosmo: both cosmo arches.
        -- Cross-compile (--target=...) skips with a soft warning since
        -- we don't have the target .a's hash in the running hull's
        -- manifest (target arch may differ from build arch).
        local arches_to_check = {}
        if is_cosmo then
            arches_to_check = {
                { arch = "cosmo-x86_64",  path = tmpdir .. "/libhull_platform.a" },
                { arch = "cosmo-aarch64", path = tmpdir .. "/.aarch64/libhull_platform.a" },
            }
        elseif opts.target then
            tool.stderr(
                "hull build: --target=" .. opts.target ..
                ": skipping platform-sig cross-check (cross-compile target\n" ..
                "       differs from running hull's arch). Use --no-verify-platform\n" ..
                "       to silence this warning.\n")
        else
            arches_to_check = {
                { arch = tool.platform_name(), path = tmpdir .. "/libhull_platform.a" },
            }
        end

        platform_arch_hashes = {}
        for _, entry in ipairs(arches_to_check) do
            if not file_exists(entry.path) then
                tool.stderr("hull build: missing platform archive for cross-check: " .. entry.path .. "\n")
                tool.rmdir(tmpdir)
                tool.exit(1)
            end
            local actual = crypto.sha256(read_file(entry.path))
            local expected = tool.platform_sig_arch_hash(entry.arch)
            if not expected then
                tool.stderr(
                    "hull build: arch '" .. entry.arch .. "' not in embedded\n" ..
                    "       platform manifest. The hull binary may be older than the\n" ..
                    "       release that publishes this arch, or this is a dev build\n" ..
                    "       with an incomplete manifest. Use --no-verify-platform to skip.\n")
                tool.rmdir(tmpdir)
                tool.exit(1)
            end
            if actual ~= expected then
                tool.stderr(
                    "hull build: libhull_platform.a hash does not match the embedded\n" ..
                    "       signed manifest for arch '" .. entry.arch .. "':\n" ..
                    "         expected: " .. expected .. "\n" ..
                    "         actual:   " .. actual .. "\n" ..
                    "       The platform archive was modified between hull install\n" ..
                    "       and now, OR this hull was rebuilt against a different\n" ..
                    "       libhull_platform.a than CI signed. Use --no-verify-platform\n" ..
                    "       to override (runtime verify will then reject the app).\n")
                tool.rmdir(tmpdir)
                tool.exit(1)
            end
            platform_arch_hashes[entry.arch] = actual
        end
    else
        -- --no-verify-platform: try to fetch the blob anyway so apps
        -- built with this flag still inherit the manifest (just without
        -- the cross-check guarantee). If there's no embedded blob, the
        -- app's package.sig.platform omits the new fields entirely.
        platform_sig_blob = tool.platform_sig_get()
    end

    -- Validate compiler matches platform (cc already resolved above)
    if platform_dir then
        local cc_data = read_file(platform_dir .. "platform_cc")
        if cc_data and opts.cc then
            local platform_cc = cc_data:match("^%s*(.-)%s*$")
            if platform_cc ~= opts.cc then
                tool.stderr("hull build: warning: --compiler " .. opts.cc ..
                    " does not match platform (built with " .. platform_cc .. ")\n")
            end
        end
    end

    -- Compile
    print("hull build: compiling with " .. tool.compiler.name() .. "...")
    local ok = tool.compiler.compile(tmpdir .. "/app_registry.c",
                                      tmpdir .. "/app_registry.o",
                                      tmpdir)
    if not ok then
        tool.stderr("hull build: compilation failed (app_registry.c)\n")
        tool.rmdir(tmpdir)
        tool.exit(1)
    end

    ok = tool.compiler.compile(tmpdir .. "/app_main.c",
                                tmpdir .. "/app_main.o",
                                nil)
    if not ok then
        tool.stderr("hull build: compilation failed (app_main.c)\n")
        tool.rmdir(tmpdir)
        tool.exit(1)
    end

    -- ── Compose features (--with / inferred) ──
    -- Composable features (docs/features_and_flavors.md): a large optional
    -- subsystem linked in from a signed feature archive rather than compiled
    -- into the base. Each feature fills a base subsystem's WEAK hook with a
    -- STRONG override -- a generated registry .o that displaces the weak default
    -- -- so the base routes to the feature's backend at runtime. Selected with
    -- --with=<name>. (Manifest inference is deliberately NOT done here:
    -- extract_app_manifest EXECUTES app.lua and re-running it corrupts the sign
    -- flow; a side-effect-free manifest read is a tracked follow-up.)
    --
    -- FEATURE_SPECS is the single source of truth per feature. A feature that
    -- contributes a backend to a base hook that returns an ARRAY (so several
    -- features could feed one hook) carries { backend, type, hook }: the compose
    -- step generates one strong collector filling that hook. A feature whose
    -- base hooks each have exactly one provider carries no backend/hook; instead
    -- `whole_archive = true` tells the compose step to force-load its archive so
    -- the strong overrides (spread across its object files, with no single
    -- backend symbol to anchor on) all get pulled. `whole_archive` is a plain
    -- per-feature LINK attribute, sibling to `cxx` -- not a second kind of
    -- feature. Other fields: `cxx` (needs a system compiler + C++ runtime) and
    -- `libs` (extra link libs). Adding a feature is one row here + a
    -- `make feature-<name>` archive + a release-pipeline entry.
    local FEATURE_SPECS = {
        -- sqlite: the vendored SQLite engine + backend + UDF + agent
        -- introspection in libhull_feature-sqlite.a (`make feature-sqlite`),
        -- filling the same hl_db_feature_backends hook. Unlike the others,
        -- SQLite is Hull's DEFAULT backend composed onto a SQLite-less DB-core
        -- base (built HL_SQLITE_FEATURE=1); docs/sqlite_feature.md, Phase B.
        -- `base_group = true` because the archive references base symbols
        -- (db_registry, the _hull_* guard, agent write_error, vfs, migrate) and
        -- the base's generated override references the archive's backend, so the
        -- compose wraps platform lib + archive in --start-group for GNU ld.
        sqlite = {
            backend    = "hl_db_backend_sqlite",
            type       = "HlDbBackend",
            hook       = "hl_db_feature_backends",
            cxx        = false,
            base_group = true,
            libs       = { darwin = {}, other = {} },
        },
        duckdb = {
            backend = "hl_db_backend_duckdb",
            type    = "HlDbBackend",
            hook    = "hl_db_feature_backends",
            cxx     = true,
            libs    = { darwin = { "-lc++" }, other = { "-lstdc++", "-ldl" } },
        },
        -- postgres: pure-C PostgreSQL wire backend in libhull_feature-postgres.a
        -- (`make feature-postgres`), filling the same hl_db_feature_backends hook
        -- as duckdb. No vendored engine / no extra link libs. `base_group = true`
        -- because the backend references base crypto (SCRAM) + tls_client
        -- (sslmode) that a DB-only app doesn't otherwise pull, so the compose must
        -- wrap the platform lib + this archive in --start-group for GNU ld.
        postgres = {
            backend    = "hl_db_backend_postgres",
            type       = "HlDbBackend",
            hook       = "hl_db_feature_backends",
            cxx        = false,
            base_group = true,
            libs       = { darwin = {}, other = {} },
        },
        -- mysql: pure-C MySQL/MariaDB wire backend in libhull_feature-mysql.a
        -- (`make feature-mysql`), one backend serving both mysql:// and
        -- mariadb://. Identical shape to postgres — fills hl_db_feature_backends,
        -- references base crypto (native/caching_sha2 auth) + tls_client
        -- (sslmode), so base_group = true (--start-group at compose on GNU ld).
        mysql = {
            backend    = "hl_db_backend_mysql",
            type       = "HlDbBackend",
            hook       = "hl_db_feature_backends",
            cxx        = false,
            base_group = true,
            libs       = { darwin = {}, other = {} },
        },
        -- gpu: wgpu-native backend, isolated in libhull_feature-gpu.a
        -- (`make feature-gpu`). Base ships the generic gpu dispatch layer +
        -- the weak hl_gpu_feature_backends hook; this fills it. C (no cxx). The
        -- frameworks / -lvulkan can't live in the .a so they're emitted here.
        gpu = {
            backend = "hl_gpu_backend_wgpu", type = "HlGpuBackend",
            hook = "hl_gpu_feature_backends", cxx = false,
            libs = { darwin = { "-framework", "Metal", "-framework", "QuartzCore",
                                "-framework", "CoreGraphics", "-framework", "Foundation" },
                     other  = { "-lvulkan" } },
        },
        -- tui: the whole TUI subsystem (cap + both runtime bridges) in
        -- libhull_feature-tui.a (`make feature-tui`). Its base hooks
        -- (hl_tui_feature_present / register_lua / register_js) each have one
        -- provider, and the strong overrides live across several object files
        -- with no single backend symbol, so the archive is whole-archive-linked
        -- (force_load) rather than pull-by-backend-symbol. Pure C, no extra libs
        -- (POSIX termios).
        tui = {
            whole_archive = true, cxx = false,
            libs = { darwin = {}, other = {} },
        },
    }
    -- SQLite auto-compose (docs/sqlite_feature.md, Phase C). SQLite is Hull's
    -- DEFAULT backend, so a DB-using app auto-composes libhull_feature-sqlite.a
    -- -- but ONLY when the target base is SQLite-less. A stock base carries the
    -- SQLite backend in-lib; composing it there would double-define the backend,
    -- so we gate on the base NOT already providing hl_db_backend_sqlite. Net:
    -- a no-op on a stock (SQLite-full) base -> byte-identical; on a
    -- HL_SQLITE_FEATURE=1 base a DB app composes SQLite and a DB-free app drops
    -- it. Inferred (not a hand-typed --with=sqlite), like hull/tui.
    if not (opts.with and opts.with.sqlite) then
        local mf = extract_app_manifest(opts.app_dir)
        local needs_sqlite = false
        if mf then
            local r = tool.modules_resolve(mf, opts.flavor, with_feature_list(opts))
            if r.ok and r.needs_sqlite then needs_sqlite = true end
        end
        if needs_sqlite then
            -- Probe the resolved base: a stock base defines hl_db_backend_sqlite,
            -- a SQLite-less (HL_SQLITE_FEATURE=1) base does not. nm may be absent
            -- (or the base unreadable); default to "has sqlite" so we never
            -- wrongly compose onto a SQLite-full base.
            local base_has_sqlite = true
            local nm_out = tool.spawn_read({ "nm", platform_lib })
            if nm_out and not nm_out:find("hl_db_backend_sqlite") then
                base_has_sqlite = false
            end
            if not base_has_sqlite then
                opts.with = opts.with or {}
                opts.with.sqlite = true
                opts.with_inferred = opts.with_inferred or {}
                opts.with_inferred.sqlite = true
                print("hull build: composing feature 'sqlite' "
                      .. "(SQLite-less base + app uses db)")
            end
        end
    end

    local features_needed = {}
    if opts.with then for f in pairs(opts.with) do features_needed[f] = true end end
    local feature_objs = {}
    local feature_libs = {}
    -- Composed-feature signature attestation (issue #114). Each composed archive
    -- is recorded as { name, sha256, domain } so sign_app can write the
    -- package.sig.gethull.composed block. `domain = "platform"` = an archive that
    -- ships INSIDE hull (runtime / http / web bindings / tui bridge), attested by
    -- the platform-key manifest; `domain = "release"` = an externally-installed
    -- --with / --flavor lib, attested by the release-key hull.sha256. The `name`
    -- must match that manifest's name column. See docs/composed_feature_signing.md.
    local composed_assets = {}
    local function record_composed(name, path, domain)
        -- Only a --sign build writes the attestation; a plain `hull build`
        -- discards composed_assets, so skip the (up to ~127 MB) hash entirely.
        if not opts.sign then return end
        -- Fail CLOSED. A malformed name would record an asset the runtime §5c
        -- can never find in the signed manifest (a signed app that refuses to
        -- boot), and an unhashable archive would silently drop out of the
        -- attestation. Both are build-integrity failures, not to be papered over.
        if type(name) ~= "string" or name == "" or name:find("..", 1, true) then
            tool.stderr("hull build: internal error: malformed composed-asset "
                        .. "name '" .. tostring(name) .. "'\n")
            tool.rmdir(tmpdir); tool.exit(1)
        end
        -- Stream-hash the archive in C (tool.sha256_file) rather than
        -- read_file + crypto.sha256: a --with feature archive can be huge
        -- (~127 MB DuckDB, the wgpu GPU lib) and slurping it into a Lua
        -- string blows the tool VM's 64 MB sandbox allocator.
        local sha = tool.sha256_file(path)
        if not sha then
            tool.stderr("hull build: cannot hash composed archive '" .. path
                        .. "' for signing (" .. name .. ")\n")
            tool.rmdir(tmpdir); tool.exit(1)
        end
        composed_assets[#composed_assets + 1] =
            { name = name, sha256 = sha, domain = domain }
    end
    -- A DB wire feature (postgres/mysql) references base symbols (crypto,
    -- tls_client) that a DB-only app doesn't otherwise pull, so on GNU ld the
    -- platform lib + feature archive must be wrapped in --start-group. Set by
    -- any composed feature with `base_group = true`.
    local needs_base_group = false
    if next(features_needed) then
        local plat = tool.platform_name and tool.platform_name() or nil
        local hull_dir = ""
        if __hull_exe then hull_dir = __hull_exe:match("(.*/)") or "" end

        -- Pass 1: validate + resolve each feature's archive, collect its link
        -- libs, and group backends by the base hook they fill (one hook per
        -- subsystem kind; multiple same-kind features share one hook + array).
        local by_hook = {}       -- hook -> { type = <ctype>, backends = { sym... } }
        local needs_cxx = false
        for fname in pairs(features_needed) do
            local spec = FEATURE_SPECS[fname]
            if not spec then
                tool.stderr("hull build: --with=" .. fname .. ": unknown feature\n")
                tool.rmdir(tmpdir); tool.exit(1)
            end
            if is_cosmo then
                tool.stderr("hull build: the '" .. fname .. "' feature is native-only "
                            .. "(not available for cosmo builds)\n")
                tool.rmdir(tmpdir); tool.exit(1)
            end
            if spec.cxx then needs_cxx = true end
            if spec.base_group then needs_base_group = true end

            -- Resolve libhull_feature-<name>.a via the shared ladder: local
            -- build dirs (`make feature-<name>`), then the signed feature cache
            -- (`hull feature install`, re-verified fail-closed against the
            -- embedded release pubkey).
            local libname = "libhull_feature-" .. fname .. ".a"
            local asset = plat and ("libhull_feature-" .. fname .. "-" .. plat .. ".a") or nil
            local lib, from
            if fname == "sqlite" then
                -- Phase D: the SQLite engine ships EMBEDDED in an
                -- HL_APP_BASE_SQLITELESS hull, so resolve it embedded-first (like
                -- the wasm core). Falls back to the local build dir / installed
                -- feature cache on a pre-Phase-D hull.
                lib, from = fcompose.resolve_sqlite_lib(tmpdir,
                                                        { hull_dir = hull_dir, plat = plat })
            else
                lib, from = fcompose.resolve_lib(libname, asset,
                                                 { hull_dir = hull_dir, plat = plat })
            end
            if not lib then
                if from == "cache-verify-failed" then
                    tool.stderr("hull build: cached " .. fname .. " feature lib could not "
                        .. "be re-verified; run `hull feature install " .. fname .. "` again\n")
                elseif opts.with_inferred and opts.with_inferred[fname] then
                    -- Auto-inferred from a manifest declaration (e.g. hull/tui):
                    -- frame the error around what the app requires, not a --with
                    -- flag the user never typed.
                    tool.stderr("hull build: this app declares hull/" .. fname
                                .. " but the '" .. fname .. "' feature is not installed\n")
                    tool.stderr("hint: `hull feature install " .. fname .. "` "
                                .. "(the app's manifest requires it), "
                                .. "or build from source: `make feature-" .. fname .. "`\n")
                else
                    tool.stderr("hull build: the '" .. fname .. "' feature lib was not found "
                                .. "(locally or in ~/.hull/feature)\n")
                    tool.stderr("hint: `hull feature install " .. fname .. "`, "
                                .. "or build from source: `make feature-" .. fname .. "`\n")
                end
                tool.rmdir(tmpdir); tool.exit(1)
            end
            local from_cache = (from == "cache")
            local dest = tmpdir .. "/" .. libname
            -- An embedded-resolved engine (Phase D: resolve_sqlite_lib extracts
            -- straight into tmpdir) is already at `dest`; copying a file onto
            -- itself truncates it to 0 bytes, so guard like the wasm compose.
            if lib ~= dest then tool.copy(lib, dest) end
            -- Attestation domain (docs/composed_feature_signing.md): an archive
            -- resolved EMBEDDED (Phase D: the SQLite engine on an
            -- HL_APP_BASE_SQLITELESS hull) ships INSIDE hull, so it is attested by
            -- the platform-key manifest (§5c FATAL) under the embedded-asset name
            -- `libhull_feature-<n>.<arch>.a`, exactly like the runtime/wasm cores.
            -- An externally-installed feature keeps the release domain + its
            -- release-asset name `libhull_feature-<n>-<arch>.a`.
            if from == "embedded" then
                record_composed("libhull_feature-" .. fname .. "."
                                .. (plat or "") .. ".a", dest, "platform")
            else
                record_composed(asset or libname, dest, "release")
            end
            local is_darwin = plat and plat:sub(1, 6) == "darwin"

            if spec.whole_archive then
                -- Force-load the whole archive: its strong overrides of the
                -- base weak hooks are spread across several object files with no
                -- single backend symbol to anchor a selective pull.
                for _, f in ipairs(fcompose.whole_archive_flags(dest, is_darwin)) do
                    feature_libs[#feature_libs + 1] = f
                end
                -- tui (issue #114, Phase D): the cap core above is runtime-agnostic;
                -- the per-runtime bridge (register_lua/_js strong override) lives in
                -- its own archive so the WRONG runtime's bridge is never pulled onto
                -- a single-runtime base. Compose the APP's runtime bridge too
                -- (embedded in hull, resolved embedded-first). Its refs to the
                -- runtime resolve because the runtime archive is composed alongside.
                if fname == "tui" and not is_cosmo then
                    local app_rt = fcompose.detect_app_rt(opts.app_dir)
                    if app_rt then
                        local tb, tfrom = fcompose.resolve_tui_rt_lib(app_rt, tmpdir,
                                          { hull_dir = hull_dir, plat = plat })
                        if not tb then
                            tool.stderr("hull build: the tui bridge for '" .. app_rt
                                .. "' (libhull_feature-tui-" .. app_rt .. ".a) was not found "
                                .. (tfrom == "cache-verify-failed"
                                    and "(cache re-verify failed)\n"
                                    or  "(normally embedded in hull)\n"))
                            tool.rmdir(tmpdir); tool.exit(1)
                        end
                        local tdest = tmpdir .. "/libhull_feature-tui-" .. app_rt .. ".a"
                        if tb ~= tdest then tool.copy(tb, tdest) end
                        -- The per-runtime tui bridge ships INSIDE hull -> platform domain.
                        record_composed("libhull_feature-tui-" .. app_rt .. "."
                                        .. (plat or "") .. ".a", tdest, "platform")
                        for _, f in ipairs(fcompose.whole_archive_flags(tdest, is_darwin)) do
                            feature_libs[#feature_libs + 1] = f
                        end
                        print("hull build: composed tui bridge '" .. app_rt .. "'")
                    end
                end
            else
                -- Backend feature: linked plainly; the generated registry's
                -- extern-&backend reference pulls the backend object.
                feature_libs[#feature_libs + 1] = dest
                by_hook[spec.hook] = by_hook[spec.hook] or { type = spec.type, backends = {} }
                local g = by_hook[spec.hook]
                g.backends[#g.backends + 1] = spec.backend
            end

            -- Per-feature link libs (C++ runtime / native frameworks / -lvulkan).
            local platlibs = is_darwin and (spec.libs.darwin or {})
                                        or  (spec.libs.other or {})
            for _, l in ipairs(platlibs) do feature_libs[#feature_libs + 1] = l end

            print("hull build: composed feature '" .. fname .. "'"
                  .. (from == "embedded" and " (embedded)"
                      or from_cache and " (~/.hull/feature)" or " (local)"))
        end

        -- A C++ feature archive cannot be linked by the embedded TinyCC (it links
        -- no C++ runtime and does not run the archive's static initializers, so a
        -- tcc-linked binary crashes at first use). Require a system compiler.
        if needs_cxx and tool.compiler.name() == "tcc" then
            tool.stderr("hull build: a composable C++ feature cannot be linked by "
                        .. "the embedded TinyCC.\n")
            tool.stderr("hint: build with a system compiler, e.g. "
                        .. "`hull build --compiler=system --with=<name>`\n")
            tool.rmdir(tmpdir); tool.exit(1)
        end

        -- Generate ONE registry filling each aggregating base hook with the
        -- STRONG override returning that kind's composed backends. Only backend
        -- features populate `by_hook`; a compose of only whole_archive features
        -- (e.g. --with=tui alone) needs no registry (its strong overrides come
        -- straight from the force-loaded archive). Self-contained (no <stddef.h>:
        -- these files compile with a bare include path; size_t comes from the
        -- compiler builtin, ABI-exact under tcc/gcc/clang).
        if next(by_hook) then
            local reg = {
                "/* Auto-generated by hull build — do not edit. */",
                "typedef __SIZE_TYPE__ size_t;",
            }
            for hook, g in pairs(by_hook) do
                reg[#reg + 1] = "typedef struct " .. g.type .. " " .. g.type .. ";"
                local addrs = {}
                for i, b in ipairs(g.backends) do
                    reg[#reg + 1] = "extern const " .. g.type .. " " .. b .. ";"
                    addrs[i] = "&" .. b
                end
                local arr = "HL_FEAT_" .. hook
                reg[#reg + 1] = "static const " .. g.type .. " *const " .. arr
                                .. "[] = { " .. table.concat(addrs, ", ") .. " };"
                reg[#reg + 1] = "const " .. g.type .. " *const *" .. hook .. "(size_t *count) {"
                reg[#reg + 1] = "    if (count) *count = " .. #g.backends .. ";"
                reg[#reg + 1] = "    return " .. arr .. ";"
                reg[#reg + 1] = "}"
            end
            reg[#reg + 1] = ""
            write_file(tmpdir .. "/feature_registry.c", table.concat(reg, "\n"))
            if not tool.compiler.compile(tmpdir .. "/feature_registry.c",
                                         tmpdir .. "/feature_registry.o", nil) then
                tool.stderr("hull build: compilation failed (feature_registry.c)\n")
                tool.rmdir(tmpdir); tool.exit(1)
            end
            feature_objs[#feature_objs + 1] = tmpdir .. "/feature_registry.o"
        end
    end

    -- Every produced app needs STRONG hl_stdlib_feature_entries() +
    -- hl_runtime_feature_factories() for its one runtime (see
    -- fcompose.gen_app_registry_c for the full why). The base defaults are weak
    -- and win first from the archive, leaving the app with an empty stdlib and
    -- no runtime; this generated object overrides both. The registry codegen +
    -- the archive-resolution ladder + the whole-archive link fragment are shared
    -- with `hull eject` via hull.feature_compose so the two paths cannot drift.
    do
        local app_rt = fcompose.detect_app_rt(opts.app_dir)
        -- Cosmo has a DUAL base (both runtimes + the toolchain registry embedded
        -- in the fat APE); a cosmo app must NOT get an app_feature_registry (it
        -- would duplicate the base's strong hooks) and does not compose a runtime
        -- archive (features are native-only). Native has a runtime-less base and
        -- composes exactly one runtime here.
        if app_rt and not is_cosmo then
            -- Reduced-flavor x composed-runtime (issue #114, Phase D). The only
            -- flavors are `full` (HTTP on) and `pure-compute` (HTTP off), both
            -- fully supported here: a pure-compute app declares no HTTP module,
            -- so needs_http (below) is false and only the pure runtime is
            -- composed onto the Keel-free base (the runtime's few web-symbol
            -- references resolve to the base http_weakstub no-ops). An unknown
            -- flavor is already rejected upstream at flavor validation.

            -- Does this app need HTTP? (issue #114, Phase C2.) The route/ws/sse
            -- decorations (app.get/…) only exist when an HTTP module is declared,
            -- so a serving app always trips an HTTP cap in the resolver. When it
            -- needs none (a pure app.main CLI / compute tool) skip composing the
            -- http core + the per-runtime web bindings: the pure runtime's few
            -- web-symbol refs are then satisfied by the base http_weakstub no-ops.
            -- Fail safe: if resolution is unavailable, compose HTTP (never
            -- under-compose and silently break serving).
            local needs_http = true
            -- needs_wasm two-signal gate (docs/wasm_feature.md, Phase 2):
            --   S2 = the app ships compute/*.wasm (catches a WASM-backed db.udf
            --        that declares no compute module — invisible to the resolver);
            --   S1 = a declared WASM cap (hull/compute), from modules_resolve.
            -- Compose the wasm feature iff either fires; a genuinely compute-free
            -- app then links zero WAMR (~256 KB smaller).
            local needs_wasm = (#compute_files > 0)
            -- needs_sqlite (Phase C.2b): the app uses a udf-capable DB, so compose
            -- the per-runtime SQLite UDF bridge (mod_db_udf), which the base runtime
            -- archive no longer carries. SQLite is guaranteed reachable here (in the
            -- base, or composed as the engine feature by the Phase C block above),
            -- so the bridge's sqlite3_* references always resolve.
            local needs_sqlite = false
            -- needs_image (docs/image_feature.md): the app declares hull/image, so
            -- compose the image codec core + the per-runtime image bridge, which the
            -- native base no longer carries. Cleanly module-inferable (the resolver's
            -- HL_MOD_CAP_IMAGE); a genuinely image-free app links zero stb (~146 KB
            -- smaller).
            local needs_image = false
            -- needs_tls (docs/tls_feature.md, a2): the app implies the TLS stack
            -- (HTTP -> https fetch / TLS serving / smtp; a net-DB sslmode). Compose
            -- libhull_feature-tls.a (mbedTLS + the crypto/tls backends) which a
            -- TLS-less base no longer carries; a plaintext app links zero mbedTLS.
            local needs_tls = false
            do
                local mf = extract_app_manifest(opts.app_dir)
                if mf then
                    local r = tool.modules_resolve(mf, opts.flavor, with_feature_list(opts))
                    if r.ok and r.needs_http ~= nil then needs_http = r.needs_http end
                    if r.ok and r.needs_wasm then needs_wasm = true end
                    if r.ok and r.needs_sqlite then needs_sqlite = true end
                    if r.ok and r.needs_image then needs_image = true end
                    if r.ok and r.needs_tls then needs_tls = true end
                end
            end

            write_file(tmpdir .. "/app_feature_registry.c",
                       fcompose.gen_app_registry_c(app_rt))
            if not tool.compiler.compile(tmpdir .. "/app_feature_registry.c",
                                         tmpdir .. "/app_feature_registry.o", nil) then
                tool.stderr("hull build: compilation failed (app_feature_registry.c)\n")
                tool.rmdir(tmpdir); tool.exit(1)
            end
            feature_objs[#feature_objs + 1] = tmpdir .. "/app_feature_registry.o"

            -- Compose the runtime. The base platform lib is RUNTIME-LESS, so the
            -- app must link exactly one runtime archive (libhull_feature-<rt>.a)
            -- to be runnable. Auto-inferred from the entry extension. Whole-
            -- archive so the entire runtime + its embedded stdlib + vendored VM
            -- are pulled (like the tui feature), not merely what the registry's
            -- factory reference reaches. The runtime references base symbols
            -- (crypto/vfs/...) resolved from the platform lib, so the GNU-ld link
            -- must --start-group them.
            local rt_lib  = "libhull_feature-" .. app_rt .. ".a"
            local rt_plat = tool.platform_name and tool.platform_name() or nil
            local rt_hull_dir = ""
            if __hull_exe then rt_hull_dir = __hull_exe:match("(.*/)") or "" end
            local rt_path, rt_from = fcompose.resolve_runtime_lib(app_rt, tmpdir,
                                     { hull_dir = rt_hull_dir, plat = rt_plat })
            if not rt_path then
                if rt_from == "cache-verify-failed" then
                    tool.stderr("hull build: cached " .. app_rt
                        .. " runtime lib could not be re-verified\n")
                else
                    tool.stderr("hull build: the '" .. app_rt .. "' runtime feature lib "
                        .. "was not found (the runtime is normally embedded in hull)\n")
                    tool.stderr("hint: build it from source with `make feature-"
                        .. app_rt .. "`\n")
                end
                tool.rmdir(tmpdir); tool.exit(1)
            end
            local rt_dest = tmpdir .. "/" .. rt_lib
            if rt_path ~= rt_dest then tool.copy(rt_path, rt_dest) end  -- already there if extracted
            -- The runtime archive ships INSIDE hull -> platform domain.
            record_composed("libhull_feature-" .. app_rt .. "."
                            .. (rt_plat or "") .. ".a", rt_dest, "platform")
            needs_base_group = true  -- runtime refs base symbols; GNU ld needs the group
            local is_darwin = rt_plat and rt_plat:sub(1, 6) == "darwin"
            for _, f in ipairs(fcompose.whole_archive_flags(rt_dest, is_darwin)) do
                feature_libs[#feature_libs + 1] = f
            end
            print("hull build: composed runtime '" .. app_rt .. "'")

            -- Compose the WASM feature (docs/wasm_feature.md, Phase 2). The native
            -- base is compute-less: the wasm caps + WAMR live in
            -- libhull_feature-wasm.a and the compute.* binding (mod_compute) in
            -- libhull_feature-wasm-<rt>.a. Composed only when needs_wasm (S1 or S2,
            -- above); a genuinely compute-free app links zero WAMR. Whole-archive
            -- both inside the --start-group.
            if needs_wasm then
            local wasm_lib = "libhull_feature-wasm.a"
            local wasm_path, wasm_from = fcompose.resolve_wasm_lib(tmpdir,
                                         { hull_dir = rt_hull_dir, plat = rt_plat })
            if not wasm_path then
                tool.stderr("hull build: the WASM feature lib (libhull_feature-wasm.a) "
                    .. "was not found "
                    .. (wasm_from == "cache-verify-failed"
                        and "(cache re-verify failed)\n" or "(normally embedded in hull)\n"))
                tool.stderr("hint: build it from source with `make feature-wasm`\n")
                tool.rmdir(tmpdir); tool.exit(1)
            end
            local wasm_dest = tmpdir .. "/" .. wasm_lib
            if wasm_path ~= wasm_dest then tool.copy(wasm_path, wasm_dest) end
            record_composed("libhull_feature-wasm." .. (rt_plat or "") .. ".a",
                            wasm_dest, "platform")
            for _, f in ipairs(fcompose.whole_archive_flags(wasm_dest, is_darwin)) do
                feature_libs[#feature_libs + 1] = f
            end

            local wasmrt_lib = "libhull_feature-wasm-" .. app_rt .. ".a"
            local wasmrt_path, wasmrt_from = fcompose.resolve_wasm_rt_lib(app_rt, tmpdir,
                                             { hull_dir = rt_hull_dir, plat = rt_plat })
            if not wasmrt_path then
                tool.stderr("hull build: the compute-bindings feature lib (" .. wasmrt_lib
                    .. ") was not found "
                    .. (wasmrt_from == "cache-verify-failed"
                        and "(cache re-verify failed)\n" or "(normally embedded in hull)\n"))
                tool.rmdir(tmpdir); tool.exit(1)
            end
            local wasmrt_dest = tmpdir .. "/" .. wasmrt_lib
            if wasmrt_path ~= wasmrt_dest then tool.copy(wasmrt_path, wasmrt_dest) end
            record_composed("libhull_feature-wasm-" .. app_rt .. "."
                            .. (rt_plat or "") .. ".a", wasmrt_dest, "platform")
            for _, f in ipairs(fcompose.whole_archive_flags(wasmrt_dest, is_darwin)) do
                feature_libs[#feature_libs + 1] = f
            end
            print("hull build: composed WASM feature + compute bridge '" .. app_rt .. "'")
            else
                print("hull build: compute-free app (no hull/compute, no "
                    .. "compute/*.wasm) - skipped the WASM feature (~256 KB smaller)")
            end

            -- Compose the per-runtime SQLite UDF bridge (Phase C.2b). The runtime
            -- archive is SQLite-free; the db.udf bindings (mod_db_udf, the sole
            -- per-runtime sqlite3_* consumer) live in libhull_feature-sqlite-<rt>.a
            -- and compose only when the app uses a udf-capable DB. A db-free app
            -- composes neither this nor the engine, so it links zero SQLite.
            if needs_sqlite then
            local sqrt_lib = "libhull_feature-sqlite-" .. app_rt .. ".a"
            local sqrt_path, sqrt_from = fcompose.resolve_sqlite_rt_lib(app_rt, tmpdir,
                                         { hull_dir = rt_hull_dir, plat = rt_plat })
            if not sqrt_path then
                tool.stderr("hull build: the SQLite udf-bridge feature lib (" .. sqrt_lib
                    .. ") was not found "
                    .. (sqrt_from == "cache-verify-failed"
                        and "(cache re-verify failed)\n" or "(normally embedded in hull)\n"))
                tool.rmdir(tmpdir); tool.exit(1)
            end
            local sqrt_dest = tmpdir .. "/" .. sqrt_lib
            if sqrt_path ~= sqrt_dest then tool.copy(sqrt_path, sqrt_dest) end
            record_composed("libhull_feature-sqlite-" .. app_rt .. "."
                            .. (rt_plat or "") .. ".a", sqrt_dest, "platform")
            for _, f in ipairs(fcompose.whole_archive_flags(sqrt_dest, is_darwin)) do
                feature_libs[#feature_libs + 1] = f
            end
            print("hull build: composed SQLite udf bridge '" .. app_rt .. "'")
            end

            -- Compose the IMAGE feature (docs/image_feature.md). The native base is
            -- image-less: the codec caps + vendored stb live in
            -- libhull_feature-image.a and the image.* binding (mod_image) in
            -- libhull_feature-image-<rt>.a. Composed only when needs_image (the app
            -- declared hull/image); a genuinely image-free app links zero stb
            -- (~146 KB smaller). Whole-archive both inside the --start-group.
            if needs_image then
            local img_lib = "libhull_feature-image.a"
            local img_path, img_from = fcompose.resolve_image_lib(tmpdir,
                                       { hull_dir = rt_hull_dir, plat = rt_plat })
            if not img_path then
                tool.stderr("hull build: the image feature lib (libhull_feature-image.a) "
                    .. "was not found "
                    .. (img_from == "cache-verify-failed"
                        and "(cache re-verify failed)\n" or "(normally embedded in hull)\n"))
                tool.stderr("hint: build it from source with `make feature-image`\n")
                tool.rmdir(tmpdir); tool.exit(1)
            end
            local img_dest = tmpdir .. "/" .. img_lib
            if img_path ~= img_dest then tool.copy(img_path, img_dest) end
            record_composed("libhull_feature-image." .. (rt_plat or "") .. ".a",
                            img_dest, "platform")
            for _, f in ipairs(fcompose.whole_archive_flags(img_dest, is_darwin)) do
                feature_libs[#feature_libs + 1] = f
            end

            local imgrt_lib = "libhull_feature-image-" .. app_rt .. ".a"
            local imgrt_path, imgrt_from = fcompose.resolve_image_rt_lib(app_rt, tmpdir,
                                           { hull_dir = rt_hull_dir, plat = rt_plat })
            if not imgrt_path then
                tool.stderr("hull build: the image-bindings feature lib (" .. imgrt_lib
                    .. ") was not found "
                    .. (imgrt_from == "cache-verify-failed"
                        and "(cache re-verify failed)\n" or "(normally embedded in hull)\n"))
                tool.rmdir(tmpdir); tool.exit(1)
            end
            local imgrt_dest = tmpdir .. "/" .. imgrt_lib
            if imgrt_path ~= imgrt_dest then tool.copy(imgrt_path, imgrt_dest) end
            record_composed("libhull_feature-image-" .. app_rt .. "."
                            .. (rt_plat or "") .. ".a", imgrt_dest, "platform")
            for _, f in ipairs(fcompose.whole_archive_flags(imgrt_dest, is_darwin)) do
                feature_libs[#feature_libs + 1] = f
            end
            print("hull build: composed image feature + image bridge '" .. app_rt .. "'")
            end

            -- Compose the TLS feature (docs/tls_feature.md, a2). A tls-less base
            -- (HL_TLS_FEATURE=1) drops mbedTLS + the crypto/tls backends; compose
            -- libhull_feature-tls.a back when the app needs TLS AND the base is
            -- actually tls-less. Probe the base: a TLS-full base defines
            -- mbedtls_ssl_handshake, a tls-less one does not -- gate on its ABSENCE
            -- so this is a no-op (byte-identical) on a stock TLS-full base. Whole-
            -- archive inside the --start-group (the archive refs base crypto/vfs +
            -- Keel's tls_mbedtls.o).
            if needs_tls then
            local base_has_tls = true
            local tls_nm = tool.spawn_read({ "nm", platform_lib })
            if tls_nm and not tls_nm:find("mbedtls_ssl_handshake") then
                base_has_tls = false
            end
            if not base_has_tls then
                local tls_lib = "libhull_feature-tls.a"
                local tls_path, tls_from = fcompose.resolve_tls_lib(tmpdir,
                                           { hull_dir = rt_hull_dir, plat = rt_plat })
                if not tls_path then
                    tool.stderr("hull build: the TLS feature lib (libhull_feature-tls.a) "
                        .. "was not found "
                        .. (tls_from == "cache-verify-failed"
                            and "(cache re-verify failed)\n" or "(normally embedded in hull)\n"))
                    tool.stderr("hint: build it from source with `make feature-tls`\n")
                    tool.rmdir(tmpdir); tool.exit(1)
                end
                local tls_dest = tmpdir .. "/" .. tls_lib
                if tls_path ~= tls_dest then tool.copy(tls_path, tls_dest) end
                record_composed("libhull_feature-tls." .. (rt_plat or "") .. ".a",
                                tls_dest, "platform")
                needs_base_group = true  -- tls archive refs base + Keel symbols
                for _, f in ipairs(fcompose.whole_archive_flags(tls_dest, is_darwin)) do
                    feature_libs[#feature_libs + 1] = f
                end
                print("hull build: composed TLS feature (tls-less base + app needs TLS)")
            end
            end

            if needs_http then
            -- Compose the HTTP feature (issue #114, Phase B). The native base is
            -- HTTP-CORE-LESS: serve.c + the runtime's web-module bindings
            -- reference the http/ws/smtp/body caps, which now live in
            -- libhull_feature-http.a. Whole-archive it inside the --start-group
            -- (the caps reference Keel + base symbols, and the runtime's web
            -- bindings reference the caps). Composed for every full-flavor native
            -- app for now; a reduced flavor fails closed earlier, and skipping it
            -- for a genuinely HTTP-free app is a later refinement.
            local http_lib = "libhull_feature-http.a"
            local http_path, http_from = fcompose.resolve_http_lib(tmpdir,
                                         { hull_dir = rt_hull_dir, plat = rt_plat })
            if not http_path then
                if http_from == "cache-verify-failed" then
                    tool.stderr("hull build: cached HTTP feature lib could not be re-verified\n")
                else
                    tool.stderr("hull build: the HTTP feature lib "
                        .. "(libhull_feature-http.a) was not found "
                        .. "(normally embedded in hull)\n")
                    tool.stderr("hint: build it from source with `make feature-http`\n")
                end
                tool.rmdir(tmpdir); tool.exit(1)
            end
            local http_dest = tmpdir .. "/" .. http_lib
            if http_path ~= http_dest then tool.copy(http_path, http_dest) end
            -- The HTTP core archive ships INSIDE hull -> platform domain.
            record_composed("libhull_feature-http." .. (rt_plat or "") .. ".a",
                            http_dest, "platform")
            for _, f in ipairs(fcompose.whole_archive_flags(http_dest, is_darwin)) do
                feature_libs[#feature_libs + 1] = f
            end
            print("hull build: composed HTTP feature")

            -- Compose the Keel event loop (docs/keel_feature.md, Phase 4.2b) when
            -- the base is Keel-less. Sentinel: hl_async_backend_keel (async_keel.o's
            -- vtable) is fully ABSENT from a Keel-less base (HL_KEEL_FEATURE=1 drops
            -- async/keel.c) and present in a Keel-full base. It is a cleaner probe
            -- than hull_serve, which serve_cli.o defines WEAK -- and macOS plain `nm`
            -- prints a weak def as `T`, indistinguishable from a strong one. When the
            -- sentinel is absent, compose libhull_feature-keel.a (serve.o + async_keel
            -- + net_keel + the server-only static/agent/test objects); a full base,
            -- which already carries them, never double-composes (no duplicate
            -- hull_serve). The composed serve.o's kl_* refs pull libkeel from the base
            -- .a on demand.
            -- Default to "present" (skip compose), flipping to compose only on a
            -- CONFIRMED nm result lacking the sentinel -- mirrors the sqlite/tls
            -- probes. If nm is absent or errors (serve_nm == nil), skipping is the
            -- safe no-op for the common Keel-FULL base: composing there would
            -- duplicate serve.o's strong hull_serve / hl_async_backend and fail the
            -- link. The Keel-less release base is only built where nm exists.
            local base_has_keel = true
            local serve_nm = tool.spawn_read({ "nm", platform_lib })
            if serve_nm and not serve_nm:find("hl_async_backend_keel") then
                base_has_keel = false
            end
            if not base_has_keel then
                local keel_lib = "libhull_feature-keel.a"
                local keel_path, keel_from = fcompose.resolve_keel_lib(tmpdir,
                                             { hull_dir = rt_hull_dir, plat = rt_plat })
                if not keel_path then
                    tool.stderr("hull build: the Keel feature lib "
                        .. "(libhull_feature-keel.a) was not found "
                        .. (keel_from == "cache-verify-failed"
                            and "(cache re-verify failed)\n" or "(normally embedded in hull)\n"))
                    tool.stderr("hint: build it from source with `make feature-keel`\n")
                    tool.rmdir(tmpdir); tool.exit(1)
                end
                local keel_dest = tmpdir .. "/" .. keel_lib
                if keel_path ~= keel_dest then tool.copy(keel_path, keel_dest) end
                record_composed("libhull_feature-keel." .. (rt_plat or "") .. ".a",
                                keel_dest, "platform")
                needs_base_group = true  -- keel archive refs base + libkeel symbols
                for _, f in ipairs(fcompose.whole_archive_flags(keel_dest, is_darwin)) do
                    feature_libs[#feature_libs + 1] = f
                end
                print("hull build: composed Keel event loop (Keel-less base)")
            end

            -- Phase C: the per-runtime web bindings (routes/dispatch/res:*/ws/sse/
            -- http-client/smtp) live in libhull_feature-http-<rt>.a, not the pure
            -- runtime archive. Whole-archive it too (its strong defs override the
            -- base http_weakstub no-ops; it references the http core caps + Keel,
            -- all inside the --start-group).
            local httprt_lib = "libhull_feature-http-" .. app_rt .. ".a"
            local httprt_path, httprt_from = fcompose.resolve_http_rt_lib(app_rt, tmpdir,
                                             { hull_dir = rt_hull_dir, plat = rt_plat })
            if not httprt_path then
                if httprt_from == "cache-verify-failed" then
                    tool.stderr("hull build: cached web-bindings lib (" .. httprt_lib
                        .. ") could not be re-verified\n")
                else
                    tool.stderr("hull build: the web-bindings feature lib "
                        .. "(" .. httprt_lib .. ") was not found "
                        .. "(normally embedded in hull)\n")
                    tool.stderr("hint: build it from source with `make feature-http-"
                        .. app_rt .. "`\n")
                end
                tool.rmdir(tmpdir); tool.exit(1)
            end
            local httprt_dest = tmpdir .. "/" .. httprt_lib
            if httprt_path ~= httprt_dest then tool.copy(httprt_path, httprt_dest) end
            -- The per-runtime web bindings ship INSIDE hull -> platform domain.
            record_composed("libhull_feature-http-" .. app_rt .. "."
                            .. (rt_plat or "") .. ".a", httprt_dest, "platform")
            for _, f in ipairs(fcompose.whole_archive_flags(httprt_dest, is_darwin)) do
                feature_libs[#feature_libs + 1] = f
            end
            print("hull build: composed web bindings '" .. app_rt .. "'")
            else
                print("hull build: HTTP-free app (app.main, no HTTP modules) - "
                    .. "skipped http core + web bindings")
            end
        end
    end

    -- Link
    print("hull build: linking...")
    local platform_a = tmpdir .. "/libhull_platform.a"
    local link_objs = {tmpdir .. "/app_main.o", tmpdir .. "/app_registry.o"}
    for _, o in ipairs(feature_objs) do link_objs[#link_objs + 1] = o end
    -- A DB wire feature's archive references base symbols (tls_client, crypto)
    -- resolved from the platform lib. On GNU ld's one-pass link a later archive
    -- can't pull back into an earlier one, so wrap the platform lib + feature
    -- archives in --start-group/--end-group (ld iterates to a fixed point).
    -- GNU ld / lld only: macOS ld64 rescans archives natively AND rejects
    -- --start-group ("unknown option"), so it's gated off for a darwin target.
    local target_is_darwin = (tool.platform_name() or ""):sub(1, 6) == "darwin"
    local group = needs_base_group and not target_is_darwin
    local link_libs = {}
    if group then link_libs[#link_libs + 1] = "-Wl,--start-group" end
    link_libs[#link_libs + 1] = platform_a
    for _, l in ipairs(feature_libs) do link_libs[#link_libs + 1] = l end
    if group then link_libs[#link_libs + 1] = "-Wl,--end-group" end
    link_libs[#link_libs + 1] = "-lm"
    link_libs[#link_libs + 1] = "-lpthread"
    ok = tool.compiler.link(opts.output, link_objs, link_libs)
    if not ok then
        tool.stderr("hull build: linking failed\n")
        tool.rmdir(tmpdir)
        tool.exit(1)
    end

    print("hull build: wrote " .. opts.output)

    -- Sign if requested
    if opts.sign then
        -- Find platform.sig. Search order:
        --   1. platform_dir — the dir we extracted the .a from
        --      (source-tree workflow: build/ on a hull-from-source).
        --   2. tmpdir/platform.sig — fallback when embedded extraction
        --      put the .a in tmpdir but the user pre-signed there.
        --   3. dirname(hull_exe)/platform.sig — when the user signed
        --      next to the hull binary itself.
        --   4. ./build/platform.sig (CWD) — sign-platform's default
        --      output dir for the source-tree workflow. Catches the
        --      `hull sign-platform plat && hull build --sign` pattern
        --      run from the same shell.
        --   5. ./platform.sig (CWD) — fallback for users who pass
        --      --dir=. to sign-platform.
        local platform_sig_path = nil
        if platform_dir then
            platform_sig_path = platform_dir .. "platform.sig"
        end
        if not platform_sig_path or not file_exists(platform_sig_path) then
            if file_exists(tmpdir .. "/platform.sig") then
                platform_sig_path = tmpdir .. "/platform.sig"
            end
        end
        if not platform_sig_path or not file_exists(platform_sig_path) then
            local hull_dir = ""
            if __hull_exe then
                hull_dir = __hull_exe:match("(.*/)" ) or ""
            end
            if hull_dir ~= "" and file_exists(hull_dir .. "platform.sig") then
                platform_sig_path = hull_dir .. "platform.sig"
            end
        end
        if not platform_sig_path or not file_exists(platform_sig_path) then
            if file_exists("build/platform.sig") then
                platform_sig_path = "build/platform.sig"
            elseif file_exists("./platform.sig") then
                platform_sig_path = "./platform.sig"
            end
        end

        local sign_ctx = {
            cc = cc,
            flavor = opts.flavor,
            features = with_feature_list(opts),
            binary_hash = nil,
            trampoline_hash = crypto.sha256(app_main),
            platform_sig_path = platform_sig_path,
            -- v0.1.3 platform-sig chain. Populated by the cross-check
            -- block above. nil when --no-verify-platform was passed AND
            -- this hull has no embedded blob; an empty table when the
            -- flag was passed but a blob is available; populated when
            -- the cross-check actually ran.
            platform_sig_blob   = platform_sig_blob,
            platform_arch_hashes = platform_arch_hashes,
            -- Composed-feature attestations (issue #114): every archive
            -- whole-archived / linked into this app above, tagged by domain.
            -- sign_app groups these into package.sig.gethull.composed.
            composed_assets = composed_assets,
        }

        -- Compute binary_hash (SHA256 of the linked output binary)
        local binary_data = read_file(opts.output)
        if binary_data then
            sign_ctx.binary_hash = crypto.sha256(binary_data)
        end

        sign_app(opts.app_dir, opts.sign, sign_ctx, {
            js = js_files,
            json = json_files,
            lua = lua_files,
            migrations = migration_files,
            static = static_files,
            templates = html_files,
        }, tmpdir, opts.output)
    end

    -- Cleanup
    tool.rmdir(tmpdir)
end

-- The tool dispatcher (src/hull/tool.c) invokes the returned main() only when
-- this module is the entry command it was asked to run. A module that is
-- require()'d as a dependency (e.g. by an app during manifest extraction in
-- the tool VM) hands its main() back but is never called, so it can't run
-- against the wrong argv.
return main
