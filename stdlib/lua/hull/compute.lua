--
-- hull.compute — WASM module developer tooling
--
-- Usage:
--   hull compute new <name>            Create a new WASM compute module
--   hull compute build [name]          Compile module(s) to .wasm
--   hull compute test <name>           Run test fixtures against a module
--   hull compute check <name>          Validate a .wasm module's exports
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json     = require("hull.json")
local cbuild   = require("hull.compute_build")

-- ── Embedded hull_compute.h ────────────────────────────────────────────

local HULL_COMPUTE_H = [[/*
 * hull_compute.h — Hull WASM compute module ABI header
 *
 * Freestanding header for Hull compute plugins. Provides:
 *   - Type definitions (no stdlib dependency)
 *   - Host call interface (logging, data segments)
 *   - Minimal libc replacements (memcpy, memset, memcmp, strlen)
 *   - 64KB bump allocator
 *   - Error codes and export macros
 *
 * Include this in your .c module and implement hull_process().
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HULL_COMPUTE_H
#define HULL_COMPUTE_H

/* ── ABI version ──────────────────────────────────────────────────── */

#define HULL_ABI_VERSION 1

/* ── Freestanding type definitions ────────────────────────────────── */

typedef signed int       int32_t;
typedef unsigned int     uint32_t;
typedef signed long long int64_t;
typedef unsigned long long uint64_t;
typedef unsigned int     size_t;
typedef unsigned char    uint8_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ── Export macros ─────────────────────────────────────────────────── */

#define HULL_EXPORT __attribute__((visibility("default")))
#define HULL_VERSION_EXPORT \
    HULL_EXPORT int32_t hull_version(void) { return HULL_ABI_VERSION; }

/* ── Error codes ──────────────────────────────────────────────────── */

#define HULL_OK          0
#define HULL_ERR_OUTPUT  (-2)   /* output buffer too small */
#define HULL_ERR_INPUT   (-3)   /* invalid input */
#define HULL_ERR_INTERNAL (-4)  /* internal error */

/* ── Host call import ─────────────────────────────────────────────── */

__attribute__((import_module("env"), import_name("host_call")))
extern int32_t host_call(int32_t opcode, int32_t ptr, int32_t len);

/* Host call opcodes */
#define HULL_OP_LOG       0x01
#define HULL_OP_DATA_INFO 0x02
#define HULL_OP_CALLBACK  0x10

/* ── Logging ──────────────────────────────────────────────────────── */

static inline void hull_log(const char *msg, int32_t len)
{
    host_call(HULL_OP_LOG, (int32_t)(size_t)msg, len);
}

/* ── Data segment access ──────────────────────────────────────────── */

/* Number of loaded data segments */
static inline int32_t hull_segment_count(void)
{
    return host_call(HULL_OP_DATA_INFO, -1, 0);
}

/* Get WASM address of segment (0 if not loaded) */
static inline void *hull_segment_addr(int32_t seg_id)
{
    return (void *)(size_t)host_call(HULL_OP_DATA_INFO, seg_id, 0);
}

/* Get size of segment */
static inline int32_t hull_segment_size(int32_t seg_id)
{
    return host_call(HULL_OP_DATA_INFO, seg_id, 1);
}

/* ── Minimal libc ─────────────────────────────────────────────────── */

static inline void *hull_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline void *hull_memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

static inline int hull_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

static inline size_t hull_strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

/* ── Bump allocator (64KB arena) ──────────────────────────────────── */

#define HULL_ARENA_SIZE (64 * 1024)

static uint8_t hull_arena[HULL_ARENA_SIZE];
static size_t  hull_arena_offset = 0;

static inline void *hull_alloc(size_t size)
{
    /* Align to 8 bytes */
    size_t aligned = (size + 7) & ~(size_t)7;
    if (hull_arena_offset + aligned > HULL_ARENA_SIZE) return NULL;
    void *ptr = &hull_arena[hull_arena_offset];
    hull_arena_offset += aligned;
    return ptr;
}

static inline void hull_alloc_reset(void)
{
    hull_arena_offset = 0;
}

/* ── UDF wire format (used when module is registered as a SQL function) ── */

#define HULL_UDF_OP_SCALAR    0x01
#define HULL_UDF_OP_STEP      0x01
#define HULL_UDF_OP_FINALIZE  0x02

#define HULL_UDF_TYPE_INTEGER 0x01
#define HULL_UDF_TYPE_REAL    0x02
#define HULL_UDF_TYPE_TEXT    0x03
#define HULL_UDF_TYPE_BLOB    0x04
#define HULL_UDF_TYPE_NULL    0x05

#define HULL_UDF_RESULT_VOID    0x00
#define HULL_UDF_RESULT_INTEGER 0x01
#define HULL_UDF_RESULT_REAL    0x02
#define HULL_UDF_RESULT_TEXT    0x03
#define HULL_UDF_RESULT_BLOB    0x04
#define HULL_UDF_RESULT_NULL    0x05

/* Helper: read opcode and argc from UDF input */
static inline uint8_t hull_udf_opcode(const void *in) { return ((const uint8_t *)in)[0]; }
static inline uint8_t hull_udf_argc(const void *in)   { return ((const uint8_t *)in)[1]; }

#endif /* HULL_COMPUTE_H */
]]

-- ── Module template (C) ────────────────────────────────────────────────

local function module_template_c(name)
    return string.format([[/*
 * %s.c — Hull WASM compute module
 *
 * Implement your processing logic in hull_process().
 * Input bytes arrive via in_ptr/in_len, write output to out_ptr (up to out_max).
 * Return the number of bytes written, or a negative error code.
 *
 * Build:  hull compute build %s
 * Test:   hull compute test %s
 */

#include "hull_compute.h"

HULL_VERSION_EXPORT

/*
 * hull_process — Main entry point
 *
 * Computes a simple byte-sum score (0-100) from the input.
 * Replace this with your actual processing logic.
 */
HULL_EXPORT
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    if (out_max < 1)
        return HULL_ERR_OUTPUT;

    if (in_len <= 0) {
        /* Empty input: score = 0 */
        *(uint8_t *)out_ptr = 0;
        return 1;
    }

    /* Sum all input bytes, map to 0-100 range */
    const uint8_t *input = (const uint8_t *)in_ptr;
    uint32_t sum = 0;
    for (int32_t i = 0; i < in_len; i++)
        sum += input[i];

    uint8_t score = (uint8_t)(sum %% 101);
    *(uint8_t *)out_ptr = score;
    return 1;
}
]], name, name, name)
end

-- ── Test fixtures template ─────────────────────────────────────────────

local TEST_FIXTURES = [[
[
    {"name": "basic", "input": "hello", "expect_status": 0},
    {"name": "empty input", "input": "", "expect_status": 0}
]
]]

-- ── Argument parsing ───────────────────────────────────────────────────

local function parse_args()
    local opts = {
        subcmd = nil,
        name = nil,
        lang = "c",
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--lang" then
            i = i + 1
            opts.lang = arg[i]
        elseif a:sub(1, 1) ~= "-" then
            if not opts.subcmd then
                opts.subcmd = a
            elseif not opts.name then
                opts.name = a
            end
        end
        i = i + 1
    end

    return opts
end

-- ── Validation ─────────────────────────────────────────────────────────

local function validate_module_name(name)
    if not name or not name:match("^[a-zA-Z0-9_%-]+$") then
        tool.stderr("hull compute: invalid module name '" .. tostring(name) .. "'\n")
        tool.stderr("  Names must contain only letters, digits, underscores, and hyphens.\n")
        tool.exit(1)
    end
end

-- ── Helpers ────────────────────────────────────────────────────────────
-- Compilation lookup, module discovery, and per-module clang invocation
-- live in stdlib/lua/hull/compute_build.lua so they can be reused by
-- stdlib/lua/hull/build.lua during `hull build` auto-rebuilds.

-- ── Tempdir test harness (used by cmd_test + cmd_check) ───────────────
--
-- Both `hull compute test` and `hull compute check` exercise a module by
-- spawning a tempdir Hull app containing the .wasm + a synthesized
-- app.lua + tests, then running `hull test` against it. The shared
-- shape is extracted here so the two commands differ only in what they
-- write into the app.

--- Create a tempdir, copy `compute/<name>.wasm` into it.
-- @return path to the new tempdir (caller frees with cleanup_harness)
local function setup_harness(name, wasm_path)
    local tmpdir = tool.tmpdir()
    if not tmpdir then
        tool.stderr("hull compute: failed to create temp directory\n")
        tool.exit(1)
    end
    tool.mkdir(tmpdir .. "/compute")
    tool.copy(wasm_path, tmpdir .. "/compute/" .. name .. ".wasm")
    tool.mkdir(tmpdir .. "/tests")
    return tmpdir
end

--- Run `hull test <tmpdir>`. Returns true on pass.
local function run_harness(tmpdir)
    local hull_exe = __hull_exe or "hull"
    return tool.spawn({hull_exe, "test", tmpdir})
end

--- Remove the tempdir.
local function cleanup_harness(tmpdir)
    if tmpdir then tool.rmdir(tmpdir) end
end

--- Escape a string for Lua-source embedding (used by test fixture inputs).
local function lua_escape(s)
    return s:gsub("\\", "\\\\")
            :gsub('"', '\\"')
            :gsub("\n", "\\n")
            :gsub("\r", "\\r")
end

-- ── Subcommand: new ────────────────────────────────────────────────────

local function cmd_new(name, lang)
    if not name then
        tool.stderr("Usage: hull compute new <name> [--lang c]\n")
        tool.exit(1)
    end

    validate_module_name(name)

    if lang ~= "c" then
        tool.stderr("hull compute new: only --lang c is supported\n")
        tool.exit(1)
    end

    local dir = "compute/" .. name

    if tool.file_exists(dir) then
        tool.stderr("hull compute new: directory '" .. dir .. "' already exists\n")
        tool.exit(1)
    end

    -- Create directory structure
    tool.mkdir("compute")
    tool.mkdir(dir)

    -- Write hull_compute.h
    tool.write_file(dir .. "/hull_compute.h", HULL_COMPUTE_H)

    -- Write module source
    tool.write_file(dir .. "/" .. name .. ".c", module_template_c(name))

    -- Write test fixtures
    tool.write_file(dir .. "/test_fixtures.json", TEST_FIXTURES)

    print("hull compute new: created " .. dir .. "/")
    print("  " .. dir .. "/hull_compute.h")
    print("  " .. dir .. "/" .. name .. ".c")
    print("  " .. dir .. "/test_fixtures.json")
    print("")
    print("Next steps:")
    print("  hull compute build " .. name)
    print("  hull compute test " .. name)
end

-- ── Subcommand: build ──────────────────────────────────────────────────

local function cmd_build(name)
    if name then validate_module_name(name) end

    -- Discover all modules; filter to one if `name` was given.
    local all_modules = cbuild.discover_modules(".")
    local todo
    if name then
        for _, m in ipairs(all_modules) do
            if m.name == name then todo = { m }; break end
        end
        if not todo then
            local src = "compute/" .. name .. "/" .. name .. ".c"
            tool.stderr("hull compute build: source not found: " .. src .. "\n")
            tool.exit(1)
        end
    else
        todo = all_modules
        if #todo == 0 then
            tool.stderr("hull compute build: no modules found under compute/\n")
            tool.exit(1)
        end
    end

    local cc = cbuild.find_clang()
    if not cc then
        tool.stderr("hull compute build: clang not found\n")
        tool.stderr("  Install clang with wasm32 target support.\n")
        tool.stderr("  macOS: brew install llvm@18\n")
        tool.stderr("  Linux: apt install clang lld\n")
        tool.exit(1)
    end

    local all_ok = true
    for _, m in ipairs(todo) do
        print("hull compute build: " .. m.name)
        local ok, err = cbuild.compile_module(cc, m)
        if not ok then
            tool.stderr("  " .. (err or "compile failed") .. "\n")
            all_ok = false
        else
            local data = tool.read_file(m.wasm)
            if data then
                print(string.format("  -> %s (%.1f KB)", m.wasm, #data / 1024))
            end
        end
    end

    if not all_ok then tool.exit(1) end
end

-- ── Subcommand: check ──────────────────────────────────────────────────

local function cmd_check(name)
    if not name then
        tool.stderr("Usage: hull compute check <name>\n")
        tool.exit(1)
    end

    validate_module_name(name)

    local wasm_path = "compute/" .. name .. ".wasm"
    if not tool.file_exists(wasm_path) then
        tool.stderr("hull compute check: " .. wasm_path .. " not found\n")
        tool.stderr("  Run: hull compute build " .. name .. "\n")
        tool.exit(1)
    end

    -- Read and validate WASM magic number
    local data = tool.read_file(wasm_path)
    if not data or #data < 8 then
        tool.stderr("hull compute check: " .. wasm_path .. " is too small or unreadable\n")
        tool.exit(1)
    end

    -- WASM magic: \0asm (0x00 0x61 0x73 0x6d)
    local b1, b2, b3, b4 = string.byte(data, 1, 4)
    if b1 ~= 0x00 or b2 ~= 0x61 or b3 ~= 0x73 or b4 ~= 0x6d then
        tool.stderr("hull compute check: " .. wasm_path .. " is not a valid WASM module\n")
        tool.exit(1)
    end

    -- WASM version (should be 1)
    local v1, v2, v3, v4 = string.byte(data, 5, 8)
    local version = v1 + v2 * 256 + v3 * 65536 + v4 * 16777216
    if version ~= 1 then
        tool.stderr("hull compute check: unexpected WASM version: " .. version .. "\n")
        tool.exit(1)
    end

    -- Spin up a tempdir Hull app, drop in just the module, and run a
    -- tiny smoke test through `compute.call(...)`. If hull test passes,
    -- the module loads correctly in WAMR.
    local tmpdir = setup_harness(name, wasm_path)

    tool.write_file(tmpdir .. "/app.lua", string.format([[
app.get("/check", function(req, res)
    if not compute.available() then
        res:status(500):json({ error = "wasm runtime not available" })
        return
    end
    local out, err = compute.call("%s", "test")
    if err then
        res:status(500):json({ error = err })
        return
    end
    res:json({ ok = true, output_len = #out })
end)
]], name))

    tool.write_file(tmpdir .. "/tests/test_check.lua", string.format([[
test("compute.call('%s') succeeds", function()
    local res = test.get("/check")
    test.eq(res.status, 200)
    test.ok(res.json.ok, "compute.call should succeed")
end)
]], name))

    local ok = run_harness(tmpdir)
    cleanup_harness(tmpdir)

    if not ok then
        tool.stderr("hull compute check: " .. name .. " failed validation\n")
        tool.exit(1)
    end

    print("hull compute check: " .. name .. " OK")
end

-- ── Subcommand: test ───────────────────────────────────────────────────

local function cmd_test(name)
    if not name then
        tool.stderr("Usage: hull compute test <name>\n")
        tool.exit(1)
    end

    validate_module_name(name)

    local wasm_path = "compute/" .. name .. ".wasm"
    if not tool.file_exists(wasm_path) then
        tool.stderr("hull compute test: " .. wasm_path .. " not found\n")
        tool.stderr("  Run: hull compute build " .. name .. "\n")
        tool.exit(1)
    end

    -- Load test fixtures
    local fixtures_path = "compute/" .. name .. "/test_fixtures.json"
    local fixtures_data = tool.read_file(fixtures_path)
    local fixtures
    if fixtures_data then
        fixtures = json.decode(fixtures_data)
    end
    if not fixtures or #fixtures == 0 then
        -- Default fixtures if none provided
        fixtures = {
            { name = "basic", input = "hello", expect_status = 0 },
            { name = "empty input", input = "", expect_status = 0 },
        }
    end

    -- Spin up a tempdir Hull app exposing the module via /call?input=...
    -- and generate one test case per fixture entry.
    local tmpdir = setup_harness(name, wasm_path)

    local app_src = table.concat({
        "-- Auto-generated test app for compute module: " .. name,
        'app.get("/health", function(req, res) res:json({ ok = true }) end)',
        "",
        'app.get("/call", function(req, res)',
        '    local input = req.query.input or ""',
        string.format('    local out, err = compute.call("%s", input)', name),
        '    if err then',
        '        res:status(500):json({ error = err })',
        '        return',
        '    end',
        '    res:json({ ok = true, output_len = #out, output_bytes = { string.byte(out, 1, #out) } })',
        'end)',
    }, "\n") .. "\n"
    tool.write_file(tmpdir .. "/app.lua", app_src)

    local test_lines = {
        "-- Auto-generated tests for compute module: " .. name,
        "",
    }
    for _, fixture in ipairs(fixtures) do
        local fname = fixture.name or "unnamed"
        local input = fixture.input or ""
        local expect_status = fixture.expect_status or 0
        local escaped = lua_escape(input)
        test_lines[#test_lines + 1] = string.format('test("fixture: %s", function()', fname)
        test_lines[#test_lines + 1] = string.format('    local res = test.get("/call?input=%s")', escaped)
        if expect_status == 0 then
            test_lines[#test_lines + 1] = '    test.eq(res.status, 200)'
            test_lines[#test_lines + 1] = '    test.ok(res.json.ok, "compute.call should succeed")'
        else
            test_lines[#test_lines + 1] = '    test.eq(res.status, 500)'
        end
        test_lines[#test_lines + 1] = "end)"
        test_lines[#test_lines + 1] = ""
    end
    tool.write_file(tmpdir .. "/tests/test_compute.lua", table.concat(test_lines, "\n"))

    local ok = run_harness(tmpdir)
    cleanup_harness(tmpdir)

    if not ok then tool.exit(1) end
end

-- ── Subcommand: refresh-header ─────────────────────────────────────────
--
-- `hull_compute.h` is owned by Hull. The canonical version is embedded
-- in this file (HULL_COMPUTE_H above) and written to each module's dir
-- on `hull compute new`. When Hull bumps the ABI or adds a new helper,
-- existing module directories carry a stale copy.
--
-- `hull compute refresh-header [name]` overwrites the per-module copy
-- with the embedded canonical version. With no name, refreshes every
-- discovered module.

local function cmd_refresh_header(name)
    if name then validate_module_name(name) end

    local modules
    if name then
        if not tool.file_exists("compute/" .. name) then
            tool.stderr("hull compute refresh-header: compute/" .. name ..
                        "/ does not exist\n")
            tool.exit(1)
        end
        modules = { { name = name } }
    else
        modules = cbuild.discover_modules(".")
        if #modules == 0 then
            tool.stderr("hull compute refresh-header: no modules under compute/\n")
            tool.exit(1)
        end
    end

    local written = 0
    for _, m in ipairs(modules) do
        local path = "compute/" .. m.name .. "/hull_compute.h"
        tool.write_file(path, HULL_COMPUTE_H)
        print("hull compute refresh-header: " .. path)
        written = written + 1
    end
    print("hull compute refresh-header: refreshed " .. written .. " header(s)")
end

-- ── Usage ──────────────────────────────────────────────────────────────

local function print_usage()
    print("Usage: hull compute <command> [options]")
    print("")
    print("Commands:")
    print("  new <name>             Create a new WASM compute module")
    print("  build [name]           Compile module(s) to .wasm")
    print("  test <name>            Run test fixtures against a module")
    print("  check <name>           Validate a .wasm module loads correctly")
    print("  refresh-header [name]  Overwrite per-module hull_compute.h from the embedded canonical version")
    print("")
    print("Options:")
    print("  --lang c               Language for 'new' (default: c, only c supported)")
    print("")
    print("Examples:")
    print("  hull compute new score")
    print("  hull compute build score")
    print("  hull compute build          # build all modules")
    print("  hull compute test score")
    print("  hull compute check score")
end

-- ── Main ───────────────────────────────────────────────────────────────

local function main()
    local opts = parse_args()

    if not opts.subcmd then
        print_usage()
        tool.exit(1)
    end

    if opts.subcmd == "new" then
        cmd_new(opts.name, opts.lang)
    elseif opts.subcmd == "build" then
        cmd_build(opts.name)
    elseif opts.subcmd == "test" then
        cmd_test(opts.name)
    elseif opts.subcmd == "check" then
        cmd_check(opts.name)
    elseif opts.subcmd == "refresh-header" then
        cmd_refresh_header(opts.name)
    else
        tool.stderr("hull compute: unknown command '" .. opts.subcmd .. "'\n\n")
        print_usage()
        tool.exit(1)
    end
end

main()
