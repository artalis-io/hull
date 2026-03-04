--
-- hull.build — Build a standalone hull application binary
--
-- Usage: hull build [options] [app_dir]
--   --runtime lua|js|both  Runtime to include (default: lua)
--   --sign <key_file>      Sign with Ed25519 private key
--   --cc <compiler>        C compiler to use (default: cosmocc)
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
        elseif a == "--cc" then
            i = i + 1
            opts.cc = arg[i]
        elseif a == "--output" or a == "-o" then
            i = i + 1
            opts.output = arg[i]
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
            pcall(chunk)
            manifest = app.get_manifest()
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

    -- Capture compiler version
    local cc_version = nil
    if sign_ctx.cc then
        local ver_out = tool.spawn_read({sign_ctx.cc, "--version"})
        if ver_out then
            cc_version = ver_out:match("^([^\n]+)")
        end
    end

    -- Build the signed payload (canonical JSON key order)
    local payload_table = {
        binary_hash = sign_ctx.binary_hash,
        build = {
            cc = sign_ctx.cc or "cosmocc",
            cc_version = cc_version,
            flags = "-std=c11 -O2",
        },
        files = file_hashes,
        manifest = manifest,
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

    -- Generate unified app_registry.c
    local registry_c = generate_app_registry(opts.app_dir, {
        lua    = lua_files,
        js     = js_files,
        json   = json_files,
        html   = html_files,
        static = static_files,
        sql    = migration_files,
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

    -- Resolve CC early (needed for cosmo detection)
    local cc = opts.cc or tool.cc or "cosmocc"
    local is_cosmo = cc:find("cosmocc") ~= nil

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

    -- Validate CC matches platform (cc already resolved above)
    if platform_dir then
        -- Validate: warn if user --cc doesn't match what platform was built with
        local cc_data = read_file(platform_dir .. "platform_cc")
        if cc_data and opts.cc then
            local platform_cc = cc_data:match("^%s*(.-)%s*$")
            if platform_cc ~= opts.cc then
                tool.stderr("hull build: warning: --cc " .. opts.cc ..
                    " does not match platform (built with " .. platform_cc .. ")\n")
            end
        end
    end

    -- Compile
    print("hull build: compiling...")
    local ok = tool.spawn({cc, "-std=c11", "-O2", "-w", "-c",
                           "-o", tmpdir .. "/app_registry.o",
                           tmpdir .. "/app_registry.c"})
    if not ok then
        tool.stderr("hull build: compilation failed (app_registry.c)\n")
        tool.rmdir(tmpdir)
        tool.exit(1)
    end

    ok = tool.spawn({cc, "-std=c11", "-O2", "-w", "-c",
                     "-o", tmpdir .. "/app_main.o",
                     tmpdir .. "/app_main.c"})
    if not ok then
        tool.stderr("hull build: compilation failed (app_main.c)\n")
        tool.rmdir(tmpdir)
        tool.exit(1)
    end

    -- Link
    print("hull build: linking...")
    local platform_a = tmpdir .. "/libhull_platform.a"
    ok = tool.spawn({cc, "-o", opts.output,
                     tmpdir .. "/app_main.o",
                     tmpdir .. "/app_registry.o",
                     platform_a,
                     "-lm", "-lpthread"})
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
