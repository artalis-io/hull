--
-- test_compute_build.lua - the compute-source file-read check (audit 15).
--
-- compute_build.compile_module preprocesses a module with clang -E -MD and
-- refuses it when the preprocessor opened a file outside compute/<name>/,
-- #embed'ed anything, or the preprocessed text (string escapes decoded) asks
-- the assembler for .incbin / .include. Pure Lua; run by
-- tests/hull/source/test_lua_source.c. clang is stubbed: the "-E" spawn
-- writes a canned dependency list + preprocessed text into an in-memory
-- file system, and tool.realpath resolves over a fixed table. The real-clang
-- path is covered by tests/e2e_compute_dev.sh.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name) end
end

-- ── stubbed tool VM ──────────────────────────────────────────────────
local files, real, spawned, canned = {}, {}, {}, {}
tool = {
    host_os = function() return "linux" end,
    tmpdir = function() return "/tmp/hull_x" end,
    read_file = function(p) return files[p] end,
    remove_file = function(p) files[p] = nil; return true end,
    rmdir = function() return true end,
    realpath = function(p)
        if real[p] then return real[p] end
        return nil, "missing"
    end,
    spawn = function(argv)
        spawned[#spawned + 1] = argv
        local mf, out, is_e
        for i, a in ipairs(argv) do
            if a == "-E" then is_e = true end
            if a == "-MF" then mf = argv[i + 1] end
            if a == "-o" then out = argv[i + 1] end
        end
        if is_e then
            files[mf] = canned.deps
            files[out] = canned.pre
        else
            files[out] = "\0asm"
        end
        return true
    end,
}

local cb = require("hull.compute_build")

real["/app/compute/m"]                = "/app/compute/m"
real["/app/compute/m/m.c"]            = "/app/compute/m/m.c"
real["/app/compute/m/hull_compute.h"] = "/app/compute/m/hull_compute.h"
real["/app/compute/m/data.txt"]       = "/app/compute/m/data.txt"
real["/app/compute/m/blob.bin"]       = "/app/compute/m/blob.bin"
real["/app/compute/m/link.h"]         = "/home/u/.ssh/id_rsa"   -- a symlink out
real["/app/compute/m/../../secret"]   = "/app/secret"
real["/app/compute/mm/x.h"]           = "/app/compute/mm/x.h"  -- a sibling dir

local MOD = { name = "m", src = "/app/compute/m/m.c", wasm = "/app/compute/m.wasm" }

local function build(deps, pre)
    canned.deps, canned.pre = deps, pre
    spawned = {}
    files = {}
    return cb.compile_module("clang", MOD)
end

local MARKS = '# 1 "/app/compute/m/m.c"\n# 1 "/app/compute/m/hull_compute.h" 1\n'
            .. '# 2 "/app/compute/m/m.c" 2\n'

-- ── a normal module builds, from the preprocessed text ───────────────
do
    local okb, err = build(
        "/tmp/hull_x/pre.i: /app/compute/m/m.c \\\n  /app/compute/m/hull_compute.h\n",
        MARKS .. "int hull_process(void) { return 0; }\n")
    ok(okb, "normal module builds (" .. tostring(err) .. ")")
    local last = spawned[#spawned] or {}
    ok(last[#last] == "/tmp/hull_x/pre.i" and last[#last - 1] == "cpp-output",
        "the compile step builds the checked pre.i, not the source")
    ok(files["/tmp/hull_x/pre.i"] == nil, "temporary files removed")
end

-- ── a header outside the module ──────────────────────────────────────
do
    local okb, err = build(
        "/tmp/hull_x/pre.i: /app/compute/m/m.c /app/compute/m/../../secret\n",
        MARKS .. '# 1 "/app/compute/m/../../secret" 1\n')
    ok(not okb and err:find("may only include its own files", 1, true),
        "include outside the module refused")
end

-- ── a symlink inside the module that points out ──────────────────────
do
    local okb = build("x.i: /app/compute/m/m.c /app/compute/m/link.h\n",
        MARKS .. '# 1 "/app/compute/m/link.h" 1\n')
    ok(not okb, "symlinked header resolving outside refused")
end

-- ── a sibling dir sharing the prefix is not inside ───────────────────
do
    local okb = build("x.i: /app/compute/m/m.c /app/compute/mm/x.h\n",
        MARKS .. '# 1 "/app/compute/mm/x.h" 1\n')
    ok(not okb, "compute/mm is not inside compute/m")
end

-- ── an unresolvable dependency fails closed ──────────────────────────
do
    local okb = build("x.i: /app/compute/m/m.c /nowhere/x.h\n", MARKS)
    ok(not okb, "unresolvable dependency refused")
end

-- ── #embed (any spelling) of a file inside the module ────────────────
do
    -- an embedded file is a dependency with no line marker
    local okb, err = build("x.i: /app/compute/m/m.c /app/compute/m/blob.bin\n",
        MARKS .. "static const char d[] = {97, 98, 99};\n")
    ok(not okb and err:find("#embed", 1, true), "#embed inside the module refused")
end

-- ── .incbin in an #included non-header, and escaped ──────────────────
do
    local okb, err = build(
        "x.i: /app/compute/m/m.c /app/compute/m/data.txt\n",
        MARKS .. '# 1 "/app/compute/m/data.txt" 1\n'
        .. '__asm__(".section .data\\n.incbin \\"../../secret\\"\\n");\n')
    ok(not okb and err:find(".incbin", 1, true), ".incbin from an included .txt refused")

    okb = build("x.i: /app/compute/m/m.c\n",
        MARKS .. '__asm__(".section .data\\n.inc\\x62in \\"/etc/passwd\\"\\n");\n')
    ok(not okb, "hex-escaped .inc\\x62in refused")
    okb = build("x.i: /app/compute/m/m.c\n",
        MARKS .. '__asm__(".inc\\142in \\"/etc/passwd\\"");\n')
    ok(not okb, "octal-escaped .inc\\142in refused")
    okb = build("x.i: /app/compute/m/m.c\n",
        MARKS .. '__asm__(".inc\\u0062in \\"/etc/passwd\\"");\n')
    ok(not okb, "UCN-escaped .inc\\u0062in refused")
    okb = build("x.i: /app/compute/m/m.c\n",
        MARKS .. '__asm__(".in" "c" "bin \\"/etc/passwd\\"");\n')
    ok(not okb, "split .incbin across adjacent literals refused")
    okb = build("x.i: /app/compute/m/m.c\n",
        MARKS .. '__asm__(".incl\\165de \\"/etc/passwd\\"");\n')
    ok(not okb, "asm .include refused")
end

-- ── helpers ──────────────────────────────────────────────────────────
do
    local d = cb._parse_deps("out.i: a.c dir\\ with\\ space/b.h \\\n  C:\\x\\y.h c$$d.h\n")
    ok(d and d[1] == "a.c" and d[2] == "dir with space/b.h" and d[3] == "C:\\x\\y.h"
        and d[4] == "c$d.h" and #d == 4, "parse_deps: escapes, continuations, Windows paths")
    ok(cb._decode_c_string("a\\x62\\143\\u0064\\n\\\"") == "abcd\n\"", "decode_c_string")
    ok(cb._preprocessed_reads_files('int x = 1; const char *s = "include";\n') == nil,
        "plain text is not refused")
end

return { pass = pass, fail = fail }
