#!/usr/bin/env python3
# check_invariants.py - the invariant lint behind `make check-invariants`.
#
# docs/design/audit_convergence.md section 5.2. Each rule below turns one of
# CLAUDE.md's "a new X must Y" statements into a check, so a change that
# breaks an established invariant fails CI before review. The rules are
# deliberately lexical (python3 only, no libclang): a small C reader strips
# comments, blanks string literals, finds function definitions by brace
# matching and records the calls in each body. That is approximate - macros
# and #if branches are read as plain text - so every rule is tuned to fail on
# the shape the invariant forbids and to stay quiet on today's tree, with the
# remaining justified exceptions listed in scripts/invariants_allowlist.txt.
#
# Allowlist format, one entry per line:
#     INV-N  path:symbol  # reason
# `symbol` is the enclosing C function; path and symbol may be shell globs
# (`path:*` covers a whole file, `src/hull/runtime/*:luaopen_*` a family). Every
# entry needs a reason. An entry that no longer matches a finding is STALE
# and fails the run too, so the list only shrinks: fix the code, then delete
# the line.
#
# Usage:
#     check_invariants.py [--root DIR] [--rule INV-N ...] [--allowlist FILE]
#                         [--no-allowlist] [--base REF] [--diff FILE] [-v]
# Exit 0 when every selected rule passes, 1 on a finding or a stale entry,
# 2 on a usage error. See docs/invariants/README.md for the rule table.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import argparse
import fnmatch
import os
import re
import subprocess
import sys

# ---------------------------------------------------------------------------
# A small C reader
# ---------------------------------------------------------------------------

KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "do", "else", "case",
    "goto", "defined", "_Alignof", "alignof", "_Static_assert", "typeof",
    "__typeof__", "__attribute__", "__builtin_expect", "offsetof",
}


def strip_comments(text):
    """Replace comments with spaces (newlines kept); keep string literals."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            if j < 0:
                j = n
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def blank_strings(text):
    """Blank the inside of string / char literals (comments already gone)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (min(j, n) - i - 1))
            if j < n and text[j] == c:
                out.append(c)
                j += 1
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def blank_preprocessor(text):
    """Blank preprocessor lines (with their backslash continuations)."""
    lines = text.split("\n")
    out, cont = [], False
    for ln in lines:
        if cont or ln.lstrip().startswith("#"):
            cont = ln.rstrip().endswith("\\")
            out.append(" " * len(ln))
        else:
            out.append(ln)
    return "\n".join(out)


def parse_macros(text):
    """`#define NAME[(args)] body` -> {NAME: body}, continuations joined."""
    out = {}
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)"
                         r"((?:[^\n]*\\\n)*[^\n]*)", text, re.M):
        out[m.group(1)] = m.group(2).replace("\\\n", " ")
    return out


FUNC_HDR = re.compile(
    r"([A-Za-z_]\w*)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)"
    r"(?:\s*__attribute__\s*\(\(.*?\)\))?\s*$", re.S)


class Func:
    __slots__ = ("name", "path", "line", "params", "body", "body_code",
                 "start", "end", "calls")

    def __init__(self, name, path, line, params, body, body_code, start, end):
        self.name, self.path, self.line = name, path, line
        self.params, self.body, self.body_code = params, body, body_code
        self.start, self.end = start, end
        self.calls = set(m.group(1) for m in
                         re.finditer(r"\b([A-Za-z_]\w*)\s*\(", body_code)
                         if m.group(1) not in KEYWORDS)


class CFile:
    """One source file: comment-free text, a string-blanked copy, functions."""

    def __init__(self, root, rel):
        self.rel = rel
        with open(os.path.join(root, rel), encoding="utf-8",
                  errors="replace") as f:
            raw = f.read()
        self.text = strip_comments(raw)                  # strings kept
        self.code = blank_preprocessor(blank_strings(self.text))
        self.macros = parse_macros(self.text)
        self.funcs = []
        self._parse()
        self.by_name = {}
        for fn in self.funcs:
            self.by_name.setdefault(fn.name, fn)

    def line_of(self, pos):
        return self.code.count("\n", 0, pos) + 1

    def _parse(self):
        code = self.code
        depth, boundary, i, n = 0, 0, 0, len(code)
        while i < n:
            c = code[i]
            if c == "{":
                if depth == 0:
                    hdr = code[boundary:i]
                    m = FUNC_HDR.search(hdr)
                    if m and "=" not in hdr and not re.search(
                            r"\b(struct|union|enum)\s+\w*\s*$", hdr):
                        end = self._match(i)
                        name = m.group(1)
                        if name not in KEYWORDS:
                            self.funcs.append(Func(
                                name, self.rel, self.line_of(boundary + m.start(1)),
                                m.group(2), self.text[i:end + 1],
                                code[i:end + 1], i, end))
                        i = end + 1
                        boundary = i
                        continue
                depth += 1
            elif c == "}":
                depth = max(0, depth - 1)
                if depth == 0:
                    boundary = i + 1
            elif c == ";" and depth == 0:
                boundary = i + 1
            i += 1

    def _match(self, i):
        depth, n, code = 0, len(self.code), self.code
        while i < n:
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
                if depth == 0:
                    return i
            i += 1
        return n - 1

    def func_at(self, pos):
        for fn in self.funcs:
            if fn.start <= pos <= fn.end:
                return fn
        return None

    def symbol_at(self, pos):
        fn = self.func_at(pos)
        return fn.name if fn else "<file>"


class Tree:
    def __init__(self, root):
        self.root = root
        self._cache = {}

    def files(self, *dirs, exts=(".c", ".h")):
        out = []
        for d in dirs:
            base = os.path.join(self.root, d)
            if os.path.isfile(base):
                out.append(d)
                continue
            if not os.path.isdir(base):
                continue
            for dp, dn, fn in os.walk(base):
                dn[:] = sorted(x for x in dn if x not in ("build", ".git"))
                for f in sorted(fn):
                    if f.endswith(exts):
                        out.append(os.path.relpath(os.path.join(dp, f),
                                                   self.root))
        return [p.replace(os.sep, "/") for p in out]

    def cfile(self, rel):
        if rel not in self._cache:
            self._cache[rel] = CFile(self.root, rel)
        return self._cache[rel]


def closure_hits(start, local, extra, pred, depth):
    """Does `start` (a Func), or a function it calls within `depth` levels,
    satisfy `pred`? Callees resolve to the same file first (`local`), then to
    `extra` (name -> Func, functions of the related directories)."""
    seen, frontier = {start.name}, [start]
    for _ in range(depth + 1):
        nxt = []
        for fn in frontier:
            if pred(fn):
                return True
            for c in fn.calls:
                if c in seen:
                    continue
                seen.add(c)
                callee = local.get(c) or extra.get(c)
                if callee:
                    nxt.append(callee)
        frontier = nxt
    return False


def reverse_roots(cf, target_names):
    """Functions of `cf` that reach a function in `target_names` through
    same-file calls (including the targets themselves), and of those the
    roots: reached functions no other reached function calls."""
    callers = {}
    for fn in cf.funcs:
        for c in fn.calls:
            callers.setdefault(c, set()).add(fn.name)
    reached, stack = set(), list(target_names)
    while stack:
        nm = stack.pop()
        if nm in reached:
            continue
        reached.add(nm)
        stack.extend(callers.get(nm, ()))
    roots = [nm for nm in reached
             if not (callers.get(nm, set()) & reached) and nm in cf.by_name]
    return reached, roots


# ---------------------------------------------------------------------------
# Rules. Each returns a list of (symbol_id, message); symbol_id is
# "path:function" and is what an allowlist entry names.
# ---------------------------------------------------------------------------

RULES = {}


def rule(rid, title):
    def deco(fn):
        RULES[rid] = (title, fn)
        return fn
    return deco


# INV-1 -------------------------------------------------------------------
# How it decides (audit convergence phase 2, design section 6.1): every
# app-facing binding is called through the runtime's binding dispatcher,
# which charges the run the call's measured wall time past what the call
# charged itself. So a binding must be REGISTERED with the dispatcher; an
# explicit charge alone no longer satisfies the rule. Two checks:
#   (a) Registration sites. In src/hull/runtime/lua/ (budget.c, the
#       dispatcher itself, and the tool VM's mod_tool.c excepted) a raw
#       luaL_setfuncs / luaL_newlib / luaL_register is a finding: modules
#       register through hl_lua_setfuncs / hl_lua_newlib. In
#       src/hull/runtime/js/ a JS_SetPropertyFunctionList or
#       JS_SetModuleExportList outside hl_js_set_function_list is a finding:
#       QuickJS instantiates a function list with its own constructor, which
#       QuickJS HULL PATCH 0011 leaves unmarked, so its calls skip the hooks.
#       (Every JS_NewCFunction* / JS_NewCFunctionData function is marked by
#       the patch, so JS needs no per-binding check.)
#   (b) Lua bindings. A BINDING is a function in src/hull/runtime/lua/ with
#       the lua_CFunction signature `(lua_State *L)` whose address is taken
#       there. It is REGISTERED when it is a row of a luaL_Reg table that the
#       same file passes to hl_lua_setfuncs / hl_lua_newlib, or when it is
#       passed to hl_lua_pushcfunction / hl_lua_pushcclosure. Any other
#       binding (a raw lua_pushcfunction / lua_pushcclosure, a table nothing
#       registers through the dispatcher) is a finding: a metamethod the
#       dispatcher never wraps (__gc, __close), a protected-call body pushed
#       from C or a coroutine primitive is allowlisted with its reason.

LUA_SIG = re.compile(r"^\s*lua_State\s*\*\s*\w+\s*$")
INV1_LUA_SKIP = {"src/hull/runtime/lua/mod_tool.c",
                 "src/hull/runtime/lua/budget.c"}
INV1_LUA_RAW_REG = re.compile(r"\b(luaL_setfuncs|luaL_newlib|luaL_register)\s*\(")
INV1_JS_RAW_REG = re.compile(
    r"\b(JS_SetPropertyFunctionList|JS_SetModuleExportList)\s*\(")
INV1_JS_REGISTRAR = "hl_js_set_function_list"


@rule("INV-1", "every exported Lua/JS binding is registered with the binding dispatcher")
def inv1(tree, ctx):
    findings = []
    # (a) registration sites
    for p in tree.files("src/hull/runtime/lua", exts=(".c", ".h")):
        if p in INV1_LUA_SKIP:
            continue
        cf = tree.cfile(p)
        for m in INV1_LUA_RAW_REG.finditer(cf.code):
            findings.append((f"{p}:{cf.symbol_at(m.start())}",
                             f"{m.group(1)} at line {cf.line_of(m.start())} "
                             "registers bindings outside the dispatcher (use "
                             "hl_lua_setfuncs / hl_lua_newlib)"))
    for p in tree.files("src/hull/runtime/js", exts=(".c", ".h")):
        cf = tree.cfile(p)
        for m in INV1_JS_RAW_REG.finditer(cf.code):
            sym = cf.symbol_at(m.start())
            if sym == INV1_JS_REGISTRAR:
                continue
            findings.append((f"{p}:{sym}",
                             f"{m.group(1)} at line {cf.line_of(m.start())} "
                             "makes functions the dispatcher never sees (use "
                             "hl_js_set_function_list)"))
    # (b) Lua bindings
    files = [p for p in tree.files("src/hull/runtime/lua", exts=(".c", ".h"))
             if p not in INV1_LUA_SKIP]
    cfs = [tree.cfile(p) for p in files]
    registered, refs = set(), set()
    push_re = re.compile(r"\bhl_lua_push(?:cfunction|cclosure)\s*\(([^;]*)\)\s*;")
    for cf in cfs:
        code = cf.code
        tables = set(re.findall(
            r"\bhl_lua_(?:setfuncs|newlib)\s*\(\s*[^,()]+,\s*(\w+)", code))
        # A function that pushes a table's rows through the dispatcher
        # (`hl_lua_pushcclosure(L, m->func, n)` in a loop): the tables it
        # names, and those passed to it, are registered.
        pushers = [fn for fn in cf.funcs
                   if re.search(r"\bhl_lua_push(?:cfunction|cclosure)\s*\("
                                r"\s*[^,()]+,\s*\w+\s*->\s*func\b", fn.body_code)]
        for fn in pushers:
            tables.update(re.findall(r"\b(\w+)\b", fn.body_code))
            for m in re.finditer(r"\b" + re.escape(fn.name) + r"\s*\(([^;]*)\)",
                                 code):
                tables.update(re.findall(r"\b(\w+)\b", m.group(1)))
        for m in re.finditer(
                r"\bluaL_Reg\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};", cf.text,
                re.S):
            if m.group(1) in tables:
                registered.update(re.findall(
                    r"\{\s*\"[^\"]*\"\s*,\s*(\w+)\s*\}", m.group(2)))
        for m in push_re.finditer(code):
            registered.update(re.findall(r"\b(\w+)\b", m.group(1)))
        for m in re.finditer(r"\b([A-Za-z_]\w*)\b(?!\s*\()", code):
            refs.add(m.group(1))
    for cf in cfs:
        for fn in cf.funcs:
            if not LUA_SIG.match(fn.params) or fn.name not in refs:
                continue
            if fn.name in registered:
                continue
            findings.append((f"{cf.rel}:{fn.name}",
                             f"binding {fn.name} (line {fn.line}) is not "
                             "registered with the binding dispatcher "
                             "(hl_lua_setfuncs / hl_lua_newlib / "
                             "hl_lua_pushcfunction)"))
    return findings


# INV-2 -------------------------------------------------------------------
# How it decides: any `sqlite3_<name>(` call in src/hull/ outside
# src/hull/cap/ is a finding (the SQLite engine is reached through
# hl_cap_db_*). The agent's read-only readers and the udf bindings, which
# need the raw handle, are allowlisted.

@rule("INV-2", "no sqlite3_* call outside src/hull/cap/")
def inv2(tree, ctx):
    findings, seen = [], set()
    for p in tree.files("src/hull", exts=(".c", ".h")):
        if p.startswith("src/hull/cap/"):
            continue
        cf = tree.cfile(p)
        for m in re.finditer(r"\bsqlite3_\w+\s*\(", cf.code):
            sid = f"{p}:{cf.symbol_at(m.start())}"
            if sid not in seen:
                seen.add(sid)
                findings.append((sid, f"{m.group(0).rstrip('( ')} at line "
                                 f"{cf.line_of(m.start())}"))
    return findings


# INV-3 -------------------------------------------------------------------
# How it decides: in src/hull/runtime/js/, a function that PARKS calls one of
# hl_js_async_cont_create / hl_js_op_suspend / hl_cap_multipart_park. For
# every same-file root that reaches a parking function through same-file
# calls, the root (or a function it reaches) must call hl_js_db_refuse_wait,
# or - for a resume that parks again - hl_js_run_yield_check (it rolls the
# transaction back and fails the run, the same refusal at a resume).
# The definitions of the primitives themselves are not parking sites.

INV3_PARK = {"hl_js_async_cont_create", "hl_js_op_suspend",
             "hl_cap_multipart_park"}
INV3_REFUSE = {"hl_js_db_refuse_wait", "hl_js_run_yield_check"}


@rule("INV-3", "every JS operation that parks calls hl_js_db_refuse_wait")
def inv3(tree, ctx):
    findings = []
    for p in tree.files("src/hull/runtime/js", exts=(".c",)):
        cf = tree.cfile(p)
        parkers = [fn.name for fn in cf.funcs
                   if (fn.calls & INV3_PARK) and fn.name not in INV3_PARK]
        if not parkers:
            continue
        _, roots = reverse_roots(cf, parkers)
        for r in sorted(roots):
            fn = cf.by_name[r]
            if not closure_hits(fn, cf.by_name, {},
                                lambda f: bool(f.calls & INV3_REFUSE),
                                8):
                findings.append((f"{p}:{r}",
                                 f"{r} (line {fn.line}) parks without "
                                 "hl_js_db_refuse_wait"))
    return findings


# INV-4 -------------------------------------------------------------------
# How it decides, two checks over src/hull/runtime/js/ and the JS entries in
# src/hull/agent/:
#   (a) a function that ARMS a run (hl_js_budget_arm / _arm_kind / _arm_as) is an entry
#       start: it, or a function it reaches in the same file, must call
#       hl_js_entry_end_txn_check (directly or through hl_js_entry_park).
#   (b) hl_db_registry_guard_stale_txns belongs at entry START only: a
#       function that calls it must also arm the run, directly or through a
#       same-file / runtime helper up to 2 levels down (it is the entry
#       start), else it is a bare guard at an entry's end.
# hl_js_budget_arm / _arm_kind / _arm_as / hl_js_entry_end_txn_check
# themselves are the primitives and are skipped.

INV4_ARM = {"hl_js_budget_arm", "hl_js_budget_arm_kind", "hl_js_budget_arm_as"}
INV4_END = {"hl_js_entry_end_txn_check", "hl_js_entry_park"}
INV4_PRIMS = INV4_ARM | {"hl_js_entry_end_txn_check"}


@rule("INV-4", "every JS entry ends through hl_js_entry_end_txn_check")
def inv4(tree, ctx):
    findings = []
    files = tree.files("src/hull/runtime/js", exts=(".c",))
    files += [p for p in tree.files("src/hull/agent", exts=(".c",))
              if "hl_js_" in tree.cfile(p).code]
    rt_funcs = {}
    for p in tree.files("src/hull/runtime/js", exts=(".c",)):
        for fn in tree.cfile(p).funcs:
            rt_funcs.setdefault(fn.name, fn)
    for p in files:
        cf = tree.cfile(p)
        for fn in cf.funcs:
            if fn.name in INV4_PRIMS:
                continue
            if fn.calls & INV4_ARM:
                if not closure_hits(fn, cf.by_name, {},
                                    lambda f: bool(f.calls & INV4_END), 4):
                    findings.append((f"{p}:{fn.name}",
                                     f"entry {fn.name} (line {fn.line}) arms a "
                                     "run but never ends through "
                                     "hl_js_entry_end_txn_check"))
            if ("hl_db_registry_guard_stale_txns" in fn.calls
                    and not closure_hits(fn, cf.by_name, rt_funcs,
                                         lambda f: bool(f.calls & INV4_ARM),
                                         2)):
                findings.append((f"{p}:{fn.name}",
                                 f"{fn.name} (line {fn.line}) runs a bare "
                                 "hl_db_registry_guard_stale_txns outside an "
                                 "entry start"))
    return findings


# INV-5 -------------------------------------------------------------------
# How it decides: a DISK LOADER is a function in src/hull/ that calls
# hl_vfs_disk_gate_check (it re-hashes bytes it read from the app tree), or a
# function in src/hull/runtime/ that names `app_dir` and opens a file
# (fopen / open / openat / hl_fs_fopen_read / hl_fs_open*). The loader, or a
# same-file function that reaches it, must call hl_vfs_embedded: a built
# binary loads nothing from beside itself.

INV5_OPEN = re.compile(r"\b(fopen|open|openat|hl_fs_fopen_read|hl_fs_open\w*)\s*\(")


@rule("INV-5", "every disk loader checks hl_vfs_embedded")
def inv5(tree, ctx):
    findings = []
    for p in tree.files("src/hull", exts=(".c",)):
        if p == "src/hull/vfs.c":
            continue
        cf = tree.cfile(p)
        loaders = []
        for fn in cf.funcs:
            if "hl_vfs_disk_gate_check" in fn.calls:
                loaders.append(fn.name)
            elif (p.startswith("src/hull/runtime/")
                  and re.search(r"\bapp_dir\b", fn.body_code)
                  and INV5_OPEN.search(fn.body_code)):
                loaders.append(fn.name)
        if not loaders:
            continue
        reached, _ = reverse_roots(cf, loaders)
        for ld in loaders:
            r2, _ = reverse_roots(cf, [ld])
            if not any("hl_vfs_embedded" in cf.by_name[n].calls
                       for n in r2 if n in cf.by_name):
                fn = cf.by_name[ld]
                findings.append((f"{p}:{ld}",
                                 f"disk loader {ld} (line {fn.line}) never "
                                 "checks hl_vfs_embedded"))
    return findings


# INV-6 -------------------------------------------------------------------
# How it decides: a call that writes formatted text straight to the terminal
# - fprintf(stderr|stdout, ...), printf(...), fputs(x, stderr|stdout),
# fwrite(..., stderr|stdout) - is a finding when it can carry text: a
# printf-style format with a %s / %.*s / %c conversion, or an fputs / fwrite
# of anything but a string literal. Arguments that are plainly Hull's own
# (string literals, strerror(...), ...) still count: the rule cannot see
# where a char * came from, so each such function is reviewed once and
# allowlisted with the reason its text is Hull's (constants, argv, a path
# the operator typed). App, request and cloned-tree text must go through
# hl_eprintf_line / hl_log_fputs_escaped / hl_term_fwrite_escaped / log_*.
# The escapers' own definitions are skipped.

INV6_ESCAPERS = {"hl_eprintf_line", "hl_log_fputs_escaped",
                 "hl_term_fwrite_escaped", "hl_log_escape_line"}
INV6_CALL = re.compile(
    r"\b(fprintf\s*\(\s*(?:stderr|stdout)\s*,|printf\s*\(|"
    r"fputs\s*\(|fwrite\s*\()")


def _call_args(text, start):
    """Text of the argument list of the call whose '(' is at or after
    `start`, through the matching ')'."""
    i = text.index("(", start)
    depth, j = 0, i
    while j < len(text):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
        elif text[j] in "\"'":
            q = text[j]
            j += 1
            while j < len(text) and text[j] != q:
                j += 2 if text[j] == "\\" else 1
        j += 1
    return text[i + 1:]


@rule("INV-6", "terminal / log output goes through the shared escaper")
def inv6(tree, ctx):
    findings, seen = [], set()
    for p in tree.files("src/hull", exts=(".c",)):
        cf = tree.cfile(p)
        for m in INV6_CALL.finditer(cf.code):
            fn = cf.func_at(m.start())
            if fn and fn.name in INV6_ESCAPERS:
                continue
            if m.start() > 0 and (cf.code[m.start() - 1].isalnum()
                                  or cf.code[m.start() - 1] == "_"):
                continue
            args = _call_args(cf.text, m.start())
            kind = m.group(1)
            risky = False
            if kind.startswith("fputs") or kind.startswith("fwrite"):
                if not re.search(r",\s*(stderr|stdout)\s*\)?\s*$", args.strip() + ")"):
                    continue
                first = args.split(",")[0].strip()
                lit = r'"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'
                # a literal, or `cond ? "a" : "b"` choosing between literals
                risky = not (re.fullmatch(lit, first) or re.fullmatch(
                    r'[^"?]*\?\s*' + lit + r'\s*:\s*' + lit, first))
            else:
                fmts = re.findall(r'"((?:[^"\\]|\\.)*)"', args)
                risky = any(re.search(r"%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?[sc]", f)
                            for f in fmts)
            if not risky:
                continue
            sid = f"{p}:{fn.name if fn else '<file>'}"
            if sid not in seen:
                seen.add(sid)
                findings.append((sid, f"raw terminal write at line "
                                 f"{cf.line_of(m.start())}"))
    return findings


# INV-7 -------------------------------------------------------------------
# How it decides: any JS_RunGC( call in src/hull/ or tests/ outside the body
# of hl_js_gc_for_retry. A collection walks every live object; the retry
# helper charges it first.

@rule("INV-7", "no JS_RunGC outside hl_js_gc_for_retry")
def inv7(tree, ctx):
    findings, seen = [], set()
    for p in tree.files("src/hull", "tests", exts=(".c",)):
        if p.startswith("tests/invariants/"):
            continue
        cf = tree.cfile(p)
        for m in re.finditer(r"\bJS_RunGC\s*\(", cf.code):
            sym = cf.symbol_at(m.start())
            if sym == "hl_js_gc_for_retry":
                continue
            sid = f"{p}:{sym}"
            if sid not in seen:
                seen.add(sid)
                findings.append((sid, f"JS_RunGC at line {cf.line_of(m.start())}"))
    return findings


# INV-8 -------------------------------------------------------------------
# How it decides: for each sqlite3_open*( call in src/hull/ (not in a
# comment), unless its first argument is the literal ":memory:", the rest of
# the enclosing function must register the file - hl_db_live_file_add, or a
# same-file function whose body calls it - BEFORE the first statement that
# reads the database (hl_cap_db_init, hl_cap_db_guard, sqlite3_exec,
# sqlite3_prepare*, hl_migrate_run).

INV8_READ = re.compile(r"\b(hl_cap_db_init|hl_cap_db_guard|sqlite3_exec|"
                       r"sqlite3_prepare\w*|hl_migrate_run)\s*\(")


@rule("INV-8", "every SQLite open registers with hl_db_live_file_add first")
def inv8(tree, ctx):
    findings = []
    for p in tree.files("src/hull", exts=(".c",)):
        cf = tree.cfile(p)
        regs = {fn.name for fn in cf.funcs
                if "hl_db_live_file_add" in fn.calls}
        regs.add("hl_db_live_file_add")
        reg_re = re.compile(r"\b(" + "|".join(sorted(regs)) + r")\s*\(")
        for m in re.finditer(r"\bsqlite3_open\w*\s*\(", cf.code):
            args = _call_args(cf.text, m.start())
            if args.split(",")[0].strip() == '":memory:"':
                continue
            fn = cf.func_at(m.start())
            if not fn:
                continue
            rest = cf.code[m.end():fn.end]
            r = reg_re.search(rest)
            rd = INV8_READ.search(rest)
            if r and (not rd or r.start() < rd.start()):
                continue
            findings.append((f"{p}:{fn.name}",
                             f"sqlite3_open at line {cf.line_of(m.start())} is "
                             "not registered with hl_db_live_file_add before "
                             "the database is read"))
    return findings


# INV-9 -------------------------------------------------------------------
# How it decides, over src/hull/:
#   (a) lua_closethread(x, NULL) and lua_resume(co, NULL|0, ...) are always
#       findings: the resumer is the `from` (Lua patch 0004, audit 39).
#   (b) a function that calls lua_resume must also call hl_lua_resume_status
#       (every runtime resume path goes through it); the coroutine library's
#       own resume (coroutine.resume / wrap) is allowlisted.

@rule("INV-9", "no lua_closethread/lua_resume without the resumer")
def inv9(tree, ctx):
    findings, seen = [], set()
    for p in tree.files("src/hull", exts=(".c",)):
        cf = tree.cfile(p)
        for m in re.finditer(r"\blua_(closethread|resume)\s*\(", cf.code):
            args = [a.strip() for a in _call_args(cf.text, m.start()).split(",")]
            fn = cf.func_at(m.start())
            sym = fn.name if fn else "<file>"
            if len(args) >= 2 and args[1] in ("NULL", "0", "(void *)0"):
                findings.append((f"{p}:{sym}",
                                 f"lua_{m.group(1)} at line "
                                 f"{cf.line_of(m.start())} passes no resumer"))
            elif (m.group(1) == "resume" and fn
                  and "hl_lua_resume_status" not in fn.calls
                  and f"{p}:{sym}" not in seen):
                seen.add(f"{p}:{sym}")
                findings.append((f"{p}:{sym}",
                                 f"lua_resume at line {cf.line_of(m.start())} "
                                 "is not followed by hl_lua_resume_status"))
    return findings


# INV-10 ------------------------------------------------------------------
# Every vendored engine Hull edits in tree is listed in ENGINES (numbered
# markers, one docs file each) or UNNUMBERED; a submodule in SUBMODULES.
# How it decides:
#   (a) For each ENGINES entry (lua, quickjs, pledge, stb, sqlite, mbedtls),
#       the set of patch numbers used in `HULL PATCH NNNN` markers must equal
#       the set of `## Patch NNNN` headings in its docs file (a heading that
#       says "dropped" counts as retired and must not be used). A docs file
#       may be absent only while the engine carries no marker. In the engines
#       numbered since audit 43 (pledge, stb, sqlite, mbedtls) a marker
#       without a number is a finding too.
#   (b) Each patches/wamr/NNNN-*.patch must be named, number and file, in
#       docs/wamr_patches.md; and the commit vendor/wamr is pinned at (the
#       gitlink in HEAD, or a `wamr.gitlink` file in a self-test root) must
#       be the "Pinned WAMR base commit" docs/wamr_patches.md names, since
#       every patch is expressed against it. vendor/keel is Hull's own
#       library (artalis-io/keel): it is changed upstream and bumped, never
#       patched here, so there is nothing to check.
#   (c) vendor/tweetnacl carries unnumbered `HULL PATCH:` markers; at least
#       one must exist while the file differs from upstream (it always does).
#   (d) The diff of the change under test against its base (--base, else the
#       merge-base with origin/main when available; --diff FILE or a
#       `vendor.diff` in the root for the self-test) must not add or change
#       a line in a vendored tree (DIFF_DIRS) unless the hunk's added lines,
#       or the 40 lines around it in the new file, carry a `HULL PATCH`
#       marker. Hull-owned files inside those trees (HULL_OWNED) are exempt,
#       and so is a version upgrade: a diff that changes the engine's version
#       line (VERSION_LINES) replaces the tree wholesale, so its hunks are not
#       checked one by one ((a) still holds on the result).

ENGINES = (("lua", "vendor/lua", "docs/lua_patches.md"),
           ("quickjs", "vendor/quickjs", "docs/quickjs_patches.md"),
           ("pledge", "vendor/pledge", "docs/pledge_patches.md"),
           ("stb", "vendor/stb", "docs/stb_patches.md"),
           ("sqlite", "vendor/sqlite", "docs/sqlite_patches.md"),
           ("mbedtls", "vendor/mbedtls", "docs/mbedtls_patches.md"))
NUMBERED_ONLY = {"pledge", "stb", "sqlite", "mbedtls"}
DIFF_DIRS = ("lua", "quickjs", "tweetnacl", "pledge", "stb", "sqlite",
             "mbedtls")
HULL_OWNED = {
    # Hull's stb glue: the implementation TU + the size-prefixed allocator.
    "vendor/stb/stb_impl.c",
    # Hull's mbedTLS configuration (MBEDTLS_CONFIG_FILE), not upstream code.
    "vendor/mbedtls/hull_config.h",
}
VERSION_LINES = {
    "lua": r"#define\s+LUA_VERSION_RELEASE\b",
    "quickjs": r"^20\d\d-\d\d-\d\d$",
    "sqlite": r"#define\s+SQLITE_VERSION\s",
    "mbedtls": r"#define\s+MBEDTLS_VERSION_STRING\s",
    "stb": r"^/\* stb_image(_write)? - v\d",
    "pledge": r"$^",
    "tweetnacl": r"$^",
}


def _upgraded_engines(diff_text):
    """Engines whose version line the diff both removes and adds."""
    out, cur = set(), None
    rem, add = {}, {}
    for ln in diff_text.split("\n"):
        if ln.startswith("+++ ") or ln.startswith("--- "):
            t = ln[4:].strip()
            t = t[2:] if t[:2] in ("a/", "b/") else t
            if t != "/dev/null":
                m = re.match(r"vendor/([a-z]+)/", t)
                cur = m.group(1) if m else None
            continue
        if cur not in VERSION_LINES:
            continue
        pat = VERSION_LINES[cur]
        if ln.startswith("-") and re.search(pat, ln[1:]):
            rem[cur] = True
        elif ln.startswith("+") and re.search(pat, ln[1:]):
            add[cur] = True
    for e in rem:
        if add.get(e):
            out.add(e)
    return out


def _hunk_findings(tree, diff_text):
    findings = []
    upgraded = _upgraded_engines(diff_text)
    dirs = "|".join(DIFF_DIRS)
    cur, new_line, hunk_added, hunk_start = None, 0, [], 0

    def flush():
        if not cur or not hunk_added:
            return
        if any("HULL PATCH" in t for _, t in hunk_added):
            return
        path = os.path.join(tree.root, cur)
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                lines = f.read().split("\n")
        except OSError:
            lines = []
        lo = max(0, hunk_added[0][0] - 41)
        hi = min(len(lines), hunk_added[-1][0] + 40)
        if any("HULL PATCH" in ln for ln in lines[lo:hi]):
            return
        findings.append((f"{cur}:line{hunk_added[0][0]}",
                         f"vendored change at {cur}:{hunk_added[0][0]} has no "
                         "HULL PATCH marker near it"))

    for ln in diff_text.split("\n"):
        if ln.startswith("+++ "):
            flush()
            hunk_added = []
            t = ln[4:].strip()
            t = t[2:] if t.startswith("b/") else t
            m = re.match(r"vendor/(" + dirs + r")/", t)
            cur = (t if m and t not in HULL_OWNED and m.group(1) not in upgraded
                   else None)
        elif ln.startswith("@@"):
            flush()
            hunk_added = []
            m = re.search(r"\+(\d+)", ln)
            new_line = int(m.group(1)) if m else 0
        elif cur is None:
            continue
        elif ln.startswith("+"):
            hunk_added.append((new_line, ln[1:]))
            new_line += 1
        elif ln.startswith("-"):
            pass
        else:
            new_line += 1
    flush()
    return findings


def _wamr_gitlink(root):
    """The commit vendor/wamr is pinned at: a self-test root's wamr.gitlink
    file, else the gitlink in HEAD. None when neither is available."""
    fp = os.path.join(root, "wamr.gitlink")
    if os.path.exists(fp):
        return open(fp, encoding="utf-8").read().strip() or None
    try:
        r = subprocess.run(["git", "-C", root, "ls-tree", "HEAD", "vendor/wamr"],
                           capture_output=True, text=True)
    except OSError:
        return None
    m = re.match(r"160000 commit ([0-9a-f]{40})\t", r.stdout)
    return m.group(1) if r.returncode == 0 and m else None


@rule("INV-10", "vendored-engine changes carry a documented HULL PATCH marker")
def inv10(tree, ctx):
    findings = []
    for eng, vdir, doc in ENGINES:
        used = {}
        for p in tree.files(vdir, exts=(".c", ".h")):
            with open(os.path.join(tree.root, p), encoding="utf-8",
                      errors="replace") as f:
                for i, ln in enumerate(f, 1):
                    for m in re.finditer(r"HULL PATCH[ #]*(\d{4})?", ln):
                        if m.group(1):
                            used.setdefault(m.group(1), f"{p}:{i}")
                        elif eng in NUMBERED_ONLY:
                            findings.append((f"{p}:line{i}",
                                             f"{p}:{i}: {eng} HULL PATCH "
                                             "marker has no NNNN number"))
        docp = os.path.join(tree.root, doc)
        if not os.path.exists(docp):
            if used:
                findings.append((f"{doc}:*", f"{doc} is missing"))
            continue
        with open(docp, encoding="utf-8") as f:
            heads = re.findall(r"^##\s+Patch\s+(\d{4})\b(.*)$", f.read(), re.M)
        live = {n for n, rest in heads if "dropped" not in rest.lower()}
        retired = {n for n, rest in heads if "dropped" in rest.lower()}
        for n, where in sorted(used.items()):
            if n not in live:
                why = "retired" if n in retired else "not documented"
                findings.append((f"{vdir}:{n}",
                                 f"{eng} HULL PATCH {n} ({where}) is {why} in {doc}"))
        for n in sorted(live - set(used)):
            findings.append((f"{doc}:{n}",
                             f"{doc} lists Patch {n} but no vendor/{eng} "
                             "marker uses it"))
    wdoc = os.path.join(tree.root, "docs/wamr_patches.md")
    wdir = os.path.join(tree.root, "patches/wamr")
    if os.path.isdir(wdir):
        doc = open(wdoc, encoding="utf-8").read() if os.path.exists(wdoc) else ""
        for f in sorted(os.listdir(wdir)):
            m = re.match(r"(\d{4})-.*\.patch$", f)
            if m and (f not in doc or not re.search(r"\b" + m.group(1) + r"\b", doc)):
                findings.append((f"patches/wamr/{f}:*",
                                 f"patches/wamr/{f} is not listed in "
                                 "docs/wamr_patches.md"))
        pin = re.search(r"Pinned WAMR base commit:\**\s*`([0-9a-f]{40})`", doc)
        link = _wamr_gitlink(tree.root)
        if link and not pin:
            findings.append(("docs/wamr_patches.md:pin",
                             "docs/wamr_patches.md names no 'Pinned WAMR "
                             "base commit'"))
        elif link and pin and link != pin.group(1):
            findings.append(("vendor/wamr:pin",
                             f"vendor/wamr is at {link[:12]} but "
                             "docs/wamr_patches.md pins "
                             f"{pin.group(1)[:12]}: re-check every patch "
                             "against the new base and update the pin"))
    tn = tree.files("vendor/tweetnacl", exts=(".c",))
    if tn and not any("HULL PATCH" in open(os.path.join(tree.root, p),
                                           encoding="utf-8",
                                           errors="replace").read()
                      for p in tn):
        findings.append(("vendor/tweetnacl:*",
                         "vendor/tweetnacl carries no HULL PATCH marker"))
    diff_text = ctx.get("diff_text")
    if diff_text:
        findings += _hunk_findings(tree, diff_text)
    return findings


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def load_allowlist(path):
    entries, errors = {}, []
    if not path:
        return entries, errors
    with open(path, encoding="utf-8") as f:
        for i, ln in enumerate(f, 1):
            s = ln.strip()
            if not s or s.startswith("#"):
                continue
            m = re.match(r"^(INV-\d+)\s+(\S+)\s+#\s*(\S.*)$", s)
            if not m:
                errors.append(f"{path}:{i}: malformed entry (want "
                              "'INV-N path:symbol  # reason'): {s}")
                continue
            entries.setdefault(m.group(1), {})[m.group(2)] = i
    return entries, errors


def allowed(sid, alist):
    """The allowlist entry covering `sid`: an exact `path:symbol`, else the
    first entry whose path and symbol both match as shell globs (`*` and `?`;
    `path:*` covers a whole file)."""
    if sid in alist:
        return sid
    path, sym = sid.rsplit(":", 1)
    for key in alist:
        kp, _, ks = key.rpartition(":")
        if fnmatch.fnmatchcase(path, kp) and fnmatch.fnmatchcase(sym, ks):
            return key
    return None


def git_diff(root, base):
    try:
        if not base:
            r = subprocess.run(["git", "-C", root, "merge-base", "HEAD",
                                "origin/main"], capture_output=True, text=True)
            if r.returncode != 0:
                return None
            base = r.stdout.strip()
        r = subprocess.run(["git", "-C", root, "diff", "-U0", base, "--",
                            *("vendor/" + d for d in DIFF_DIRS)],
                           capture_output=True, text=True)
        return r.stdout if r.returncode == 0 else None
    except OSError:
        return None


def main(argv):
    ap = argparse.ArgumentParser(description="Hull invariant lint (docs/invariants/README.md)")
    ap.add_argument("--root", default=os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
    ap.add_argument("--rule", action="append", default=[])
    ap.add_argument("--allowlist")
    ap.add_argument("--no-allowlist", action="store_true")
    ap.add_argument("--base")
    ap.add_argument("--diff")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)

    rules = a.rule or sorted(RULES, key=lambda r: int(r.split("-")[1]))
    for r in rules:
        if r not in RULES:
            print(f"check-invariants: unknown rule {r}", file=sys.stderr)
            return 2
    alpath = None if a.no_allowlist else (
        a.allowlist or os.path.join(a.root, "scripts/invariants_allowlist.txt"))
    if alpath and not os.path.exists(alpath):
        alpath = None
    alist, errors = load_allowlist(alpath)

    ctx = {}
    vdiff = os.path.join(a.root, "vendor.diff")
    if a.diff:
        ctx["diff_text"] = open(a.diff, encoding="utf-8").read()
    elif os.path.exists(vdiff):
        ctx["diff_text"] = open(vdiff, encoding="utf-8").read()
    elif "INV-10" in rules:
        ctx["diff_text"] = git_diff(a.root, a.base)

    tree = Tree(a.root)
    failed = bool(errors)
    for e in errors:
        print(e)
    for rid in rules:
        title, fn = RULES[rid]
        found = fn(tree, ctx)
        rl = alist.get(rid, {})
        used, bad = set(), []
        for sid, msg in found:
            hit = allowed(sid, rl)
            if hit:
                used.add(hit)
            else:
                bad.append((sid, msg))
        stale = sorted(set(rl) - used) if not a.rule or rid in a.rule else []
        status = "ok" if not bad and not stale else "FAIL"
        print(f"{rid} {status:4s} {title} ({len(found)} sites; "
              f"{len(found) - len(bad)} covered by {len(used)} allowlist "
              "entries)")
        for sid, msg in bad:
            print(f"  {rid} {sid}: {msg}")
        for sid in stale:
            print(f"  {rid} {sid}: stale allowlist entry "
                  f"({alpath}:{rl[sid]}) - nothing matches it any more; "
                  "delete the line")
        if a.verbose:
            for sid, msg in found:
                if allowed(sid, rl):
                    print(f"    allowlisted {sid}: {msg}")
        if bad or stale:
            failed = True
    if failed:
        print("check-invariants: FAIL. Fix the code, or - for a justified "
              "exception - add 'INV-N path:symbol  # reason' to "
              "scripts/invariants_allowlist.txt (docs/invariants/README.md).")
        return 1
    print("check-invariants: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
