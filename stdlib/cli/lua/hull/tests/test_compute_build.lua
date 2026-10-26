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
    is_symlink = function(p) return false end,
    realpath = function(p)
        if real[p] then return real[p] end
        return nil, "missing"
    end,
    spawn = function(argv)
        spawned[#spawned + 1] = argv
        local mf, out, is_e, df
        for i, a in ipairs(argv) do
            if a == "-E" then is_e = true end
            if a == "-MF" then mf = argv[i + 1] end
            if a == "-o" then out = argv[i + 1] end
            if a == "-dependency-file" then df = argv[i + 2] end -- -Xclang pair
        end
        if is_e then
            files[mf] = canned.deps
            files[out] = canned.pre
        else
            files[out] = "\0asm"
            if df then files[df] = canned.deps2 or "pre: /tmp/hull_x/pre.i\n" end
        end
        return true
    end,
}

local cb = require("hull.compute_build")

real["/app/compute/m"]                = "/app/compute/m"
real["/tmp/hull_x/pre.i"]             = "/tmp/hull_x/pre.i"
real["/abs/secret"]                   = "/abs/secret"
real["/app/compute/m/m.c"]            = "/app/compute/m/m.c"
real["/app/compute/m/hull_compute.h"] = "/app/compute/m/hull_compute.h"
real["/app/compute/m/data.txt"]       = "/app/compute/m/data.txt"
real["/app/compute/m/blob.bin"]       = "/app/compute/m/blob.bin"
real["/app/compute/m/link.h"]         = "/home/u/.ssh/id_rsa"   -- a symlink out
real["/app/compute/m/../../secret"]   = "/app/secret"
real["/app/compute/mm/x.h"]           = "/app/compute/mm/x.h"  -- a sibling dir

local MOD = { name = "m", src = "/app/compute/m/m.c", wasm = "/app/compute/m.wasm" }

local function build(deps, pre, deps2)
    canned.deps, canned.pre, canned.deps2 = deps, pre, deps2
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

-- ── any inline assembly (audit 29) ──────────────────────────────────
--
-- The assembler's macro language builds `.incbin` at assembly time
-- (`.macro m a` / `.inc\()\a` / `m bin`), out of reach of any text scan.
do
    local DEPS = "x.i: /app/compute/m/m.c\n"
    local okb, err = build(DEPS, MARKS
        .. '__asm__(".macro m a\\n.inc\\\\()\\\\a \\"/home/u/.ssh/id_rsa\\"\\n.endm\\nm bin\\n");\n')
    ok(not okb and err:find("inline assembly", 1, true),
        "asm macro building .incbin refused (" .. tostring(err) .. ")")
    ok(#spawned == 1, "no compile step after inline assembly")
    okb = build(DEPS, MARKS .. 'void f(void) { asm volatile(""); }\n')
    ok(not okb, "bare asm refused")
    okb = build(DEPS, MARKS .. 'void f(void) { __asm (""); }\n')
    ok(not okb, "__asm refused")
    okb = build(DEPS, MARKS .. 'int x __asm__("y");\n')
    ok(not okb, "asm label refused")
    okb = build(DEPS, MARKS .. 'void f(void) { __a\\\nsm__(""); }\n')
    ok(not okb, "asm keyword split by a line splice refused")
    ok(cb._preprocessed_reads_files('const char *s = "asm __asm__";\nint wasm, asm_x, my_asm;\n') == nil,
        "asm inside a literal or as part of an identifier is not refused")
end

-- ── a directive made by a macro survives -E (audit 16) ─────────────
--
-- `#define E # embed "/abs/secret"` and a line `E` leave ` # embed ...` in
-- pre.i; the cpp-output compile step runs it. deps.d lists only the source.
do
    local DEPS = "x.i: /app/compute/m/m.c\n"
    local okb, err = build(DEPS,
        MARKS .. 'const char a[] = {\n # embed "/abs/secret"\n, 0};\n')
    ok(not okb and err:find("preprocessor directive", 1, true),
        "macro-made # embed refused (" .. tostring(err) .. ")")
    ok(#spawned == 1, "no compile step after a macro-made directive")
    okb = build(DEPS, MARKS .. ' # include "/abs/secret"\nint x;\n')
    ok(not okb, "macro-made # include refused")
    okb = build(DEPS, MARKS .. '%: include "/abs/secret"\n')
    ok(not okb, "macro-made %: digraph directive refused")
    okb = build(DEPS, MARKS .. '%\\\n: include "/abs/secret"\n')
    ok(not okb, "directive split by a line splice refused")
    ok(cb._preprocessed_directive('int x; \\\n# include "x"\n') == nil,
        "a # spliced onto a line's end is not a directive")
    okb = build(DEPS, MARKS .. '#pragma GCC dependency "/abs/secret"\n')
    ok(not okb, "#pragma GCC dependency refused")
    okb = build(DEPS, MARKS .. '_Pragma("GCC dependency \\"/abs/secret\\"")\n')
    ok(not okb, "_Pragma left in pre.i refused")
    okb = build(DEPS, MARKS .. '# 7 "/app/compute/m/m.c" 2 3\n#pragma clang loop unroll(enable)\n'
        .. '#pragma clang diagnostic push\n#pragma GCC diagnostic pop\n#pragma pack(push, 1)\n'
        .. '#pragma unroll\nint hull_process(void) { return 0; }\n')
    ok(okb, "line markers and loop / diagnostic / pack pragmas allowed")
    ok(cb._preprocessed_directive('# line 3 "x"\n') ~= nil, "#line refused")

    -- defence in depth: the compile step's own dependency list
    okb, err = build(DEPS, MARKS .. "int x;\n",
        "pre: /tmp/hull_x/pre.i /abs/secret\n")
    ok(not okb and err:find("compile step read", 1, true),
        "compile step reading another file refused (" .. tostring(err) .. ")")
    ok(files["/app/compute/m.wasm"] == nil, "its output removed")
    okb = build(DEPS, MARKS .. "int x;\n", "")
    ok(not okb, "no compile-step dependency list fails closed")
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

-- ── discovered module names follow the `compute new` rule (audit 18) ─
do
    local tree = {
        "/app/compute/good_mod-1/good_mod-1.c",
        "/app/compute/bad.name/bad.name.c",
        "/app/compute/sp ace/sp ace.c",
        "/app/compute/a$(id)/a$(id).c",
    }
    tool.file_exists = function(p) return p == "/app/compute" end
    tool.find_files = function() return tree end
    local mods, invalid = cb.discover_modules("/app")
    ok(#mods == 1 and mods[1].name == "good_mod-1", "a valid name is discovered")
    ok(#invalid == 3 and invalid[1] == "a$(id)" and invalid[2] == "bad.name"
        and invalid[3] == "sp ace", "invalid names are reported, not discovered")
    ok(cb.valid_module_name("Ab_9-z") and not cb.valid_module_name("")
        and not cb.valid_module_name("a/b") and not cb.valid_module_name(".."),
        "valid_module_name")

    -- build_all compiles nothing when an invalid name is present.
    spawned = {}
    local r = cb.build_all("/app", "force")
    ok(#r.errors == 3 and #spawned == 0, "build_all refuses, nothing spawned")
    ok(r.errors[2].err:find("invalid compute module name 'bad.name'", 1, true) ~= nil,
        "the refusal names the module")

    -- A symlinked compute/ is not walked: through the link, clang read and
    -- wrote outside the app (audit 21).
    tool.is_symlink = function(p) return p == "/app/compute" end
    mods, invalid = cb.discover_modules("/app")
    ok(#mods == 0 and #invalid == 0, "a symlinked compute/ finds nothing")
    tool.is_symlink = function() return false end
end

return { pass = pass, fail = fail }
