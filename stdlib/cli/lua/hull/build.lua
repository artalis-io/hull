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

-- ── Argument parsing ─────────────────────────────────────────────────

local function parse_args()
    local opts = {
        runtime = "lua",
        sign = nil,
        cc = nil,         -- resolved from tool.cc (set by C, default cosmocc)
        output = nil,
        app_dir = ".",
        aot = true,       -- AOT compile WASM modules (--no-aot to disable)
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
        elseif a == "--no-build-compute" then
            opts.build_compute = false
        elseif a == "--target" then
            i = i + 1
            opts.target = arg[i]
        elseif a == "--no-verify-platform" then
            opts.verify_platform = false
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

-- List all files recursively in a directory
local function find_all_files(dir)
    return tool.find_files(dir, "*")
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
        local varname = var_prefix .. rel:gsub("[/.]", "_")

        parts[#parts + 1] = xxd_data(varname, data)
        parts[#parts + 1] = ""

        entries[#entries + 1] = string.format(
            '    { "%s", %s, sizeof(%s) },', entry_name, varname, varname)
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

    -- AOT-compiled compute modules (generated in tmpdir, explicit entry names)
    for _, item in ipairs(files.compute_aot or {}) do
        local data = read_file(item.path)
        if data then
            local varname = "aot_" .. item.entry_name:gsub("[/.]", "_")
            parts[#parts + 1] = xxd_data(varname, data)
            parts[#parts + 1] = ""
            entries[#entries + 1] = string.format(
                '    { "%s", %s, sizeof(%s) },', item.entry_name, varname, varname)
        end
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

local function sign_app(app_dir, key_file, sign_ctx, files)
    local key_data = read_file(key_file)
    if not key_data then
        tool.stderr("hull build: cannot read key file: " .. key_file .. "\n")
        tool.exit(1)
    end
    local sk_hex = key_data:match("^(%x+)")
    if not sk_hex or #sk_hex ~= 128 then
        tool.stderr("hull build: invalid key file format\n")
        tool.exit(1)
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

    -- Execute app to capture manifest
    local manifest = nil
    local entry = app_dir .. "/app.lua"
    if file_exists(entry) then
        local chunk = tool.loadfile(entry)
        if chunk then
            local ok, err = pcall(chunk)
            if ok then
                manifest = app.get_manifest()
            else
                tool.stderr("hull build: warning: manifest extraction failed: " .. tostring(err) .. "\n")
            end
        end
    end

    -- Resolve the manifest's modules block against the canonical registry.
    -- The result is the full set of admitted modules (declared + intrinsic
    -- core), persisted into package.sig as `modules_resolved`. Auditors can
    -- then read the bundle and see exactly which first-party module surface
    -- the app shipped with, without executing any code. The signature covers
    -- this field, so tampering invalidates the package.
    local modules_resolved = nil
    if manifest then
        local r = tool.modules_resolve(manifest)
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
                for _, wasm_path in ipairs(wasm_only) do
                    local rel = wasm_path:sub(#opts.app_dir + 2) -- e.g. "compute/score.wasm"
                    for _, arch in ipairs(targets) do
                        local aot_name = rel:gsub("%.wasm$", ".aot." .. arch) -- "compute/score.aot.x86_64"
                        local aot_path = tmpdir .. "/" .. aot_name
                        tool.mkdir(tmpdir .. "/compute")

                        local mem64 = is_memory64_wasm(wasm_path)
                        local wamrc_args = {wamrc, "--target=" .. arch, "-o", aot_path, wasm_path}
                        if mem64 then
                            table.insert(wamrc_args, 3, "--enable-memory64")
                        end
                        print("hull build: AOT " .. rel .. " -> " .. arch ..
                              (mem64 and " (memory64)" or ""))
                        local ok = tool.spawn(wamrc_args)
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
                    print("hull build: " .. #compute_aot .. " AOT module(s) compiled")
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

    -- Generate app_main.c
    local app_main = [[
extern int hull_main(int argc, char **argv);
int main(int argc, char **argv) { return hull_main(argc, argv); }
]]
    write_file(tmpdir .. "/app_main.c", app_main)

    -- Extract platform library (if embedded)
    local platform_extracted = false
    local platform_lib = tmpdir .. "/libhull_platform.a"

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

    -- Link
    print("hull build: linking...")
    local platform_a = tmpdir .. "/libhull_platform.a"
    ok = tool.compiler.link(opts.output,
                             {tmpdir .. "/app_main.o",
                              tmpdir .. "/app_registry.o"},
                             {platform_a, "-lm", "-lpthread"})
    if not ok then
        tool.stderr("hull build: linking failed\n")
        tool.rmdir(tmpdir)
        tool.exit(1)
    end

    print("hull build: wrote " .. opts.output)

    -- Sign if requested
    if opts.sign then
        -- Find platform.sig alongside the platform library
        local platform_sig_path = nil
        if platform_dir then
            platform_sig_path = platform_dir .. "platform.sig"
        end
        -- Also check tmpdir (extracted embedded builds)
        if not platform_sig_path or not file_exists(platform_sig_path) then
            if file_exists(tmpdir .. "/platform.sig") then
                platform_sig_path = tmpdir .. "/platform.sig"
            end
        end
        -- Also check hull binary directory (embedded platform may not set platform_dir)
        if not platform_sig_path or not file_exists(platform_sig_path) then
            local hull_dir = ""
            if __hull_exe then
                hull_dir = __hull_exe:match("(.*/)" ) or ""
            end
            if hull_dir ~= "" and file_exists(hull_dir .. "platform.sig") then
                platform_sig_path = hull_dir .. "platform.sig"
            end
        end

        local sign_ctx = {
            cc = cc,
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
        })
    end

    -- Cleanup
    tool.rmdir(tmpdir)
end

main()
