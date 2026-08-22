#!/bin/sh
# e2e_path_parity.sh: hull.path lexical semantics + Lua/JS parity.
#
# hull.path is PURE lexical name manipulation (no filesystem authority). This
# runs the required case matrix (normalize/join/dirname/basename/extension/stem/
# is_absolute/relative/is_within), the algebraic invariants, and the
# security-boundary cases through BOTH runtimes with an IDENTICAL expected vector
# per app (so the two implementations cannot silently diverge). Each app then
# prints a COMPREHENSIVE fingerprint that exercises EVERY operation; the two
# fingerprints must match byte-for-byte AND equal the golden vector, so the
# parity claim covers all nine ops, not just those with duplicated asserts.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

# ── Lua app ───────────────────────────────────────────────────────────────────
cat > "$WD/p.lua" <<'LUA'
local path = require("hull.path")
app.manifest({ modules = { "hull/path@1" } })

local fails = {}
local function eq(got, want, label)
    if got ~= want then
        fails[#fails + 1] = string.format("%s: got %q want %q", label, tostring(got), tostring(want))
    end
end
local function b(v) return v and "T" or "F" end

app.main(function(ctx)
    -- normalize
    eq(path.normalize("foo//bar"), "foo/bar", "n1")
    eq(path.normalize("foo/./bar"), "foo/bar", "n2")
    eq(path.normalize("foo/a/../bar"), "foo/bar", "n3")
    eq(path.normalize("/a/../../b"), "/b", "n4 abs-clamp")
    eq(path.normalize("../../foo"), "../../foo", "n5 rel-dotdot")
    eq(path.normalize("/"), "/", "n6")
    eq(path.normalize("."), ".", "n7")
    eq(path.normalize(""), ".", "n8 empty")
    eq(path.normalize("foo/"), "foo", "n9 trailing")
    eq(path.normalize("/foo/"), "/foo", "n10")
    eq(path.normalize("foo/.."), ".", "n11")
    eq(path.normalize(".."), "..", "n12")
    eq(path.normalize("/foo/../bar"), "/bar", "n13")
    -- join (absolute component RESETS the accumulator)
    eq(path.join("foo", "bar"), "foo/bar", "j1")
    eq(path.join("foo/", "bar"), "foo/bar", "j2")
    eq(path.join("foo", ".", "bar"), "foo/bar", "j3")
    eq(path.join("src", "plugins", "foo.lua"), "src/plugins/foo.lua", "j4")
    eq(path.join(), ".", "j5 empty")
    eq(path.join("", "foo", ""), "foo", "j6 empties")
    eq(path.join("a", "/b"), "/b", "j7 abs-reset")
    eq(path.join("/a", "/b"), "/b", "j8 abs-reset2")
    eq(path.join("a", "b/", "/c/d"), "/c/d", "j9 abs-reset-mid")
    -- dirname
    eq(path.dirname("/foo/bar.txt"), "/foo", "d1")
    eq(path.dirname("/foo"), "/", "d2")
    eq(path.dirname("foo"), ".", "d3")
    eq(path.dirname("foo/bar"), "foo", "d4")
    eq(path.dirname("/"), "/", "d5")
    -- basename
    eq(path.basename("/foo/bar.txt"), "bar.txt", "b1")
    eq(path.basename("/foo/bar/"), "bar", "b2")
    eq(path.basename("foo"), "foo", "b3")
    eq(path.basename("/"), "/", "b4")
    -- extension
    eq(path.extension("foo.txt"), ".txt", "e1")
    eq(path.extension("foo.tar.gz"), ".gz", "e2")
    eq(path.extension("foo"), "", "e3")
    eq(path.extension(".gitignore"), "", "e4 dotfile")
    eq(path.extension("a.b.c"), ".c", "e5")
    -- stem
    eq(path.stem("/foo/archive.tar.gz"), "archive.tar", "s1")
    eq(path.stem(".gitignore"), ".gitignore", "s2 dotfile")
    eq(path.stem("foo"), "foo", "s3")
    -- is_absolute
    eq(b(path.is_absolute("/foo/bar")), "T", "a1")
    eq(b(path.is_absolute("foo")), "F", "a2")
    -- relative
    eq(path.relative("/a/b", "/a/c/file.txt"), "../c/file.txt", "r1")
    eq(path.relative("/a", "/a"), ".", "r2")
    eq(path.relative("src", "src/plugins/foo.lua"), "plugins/foo.lua", "r3")
    -- is_within (component-aware, normalized; NOT a security check)
    eq(b(path.is_within("/a", "/a")), "T", "w1 eq")
    eq(b(path.is_within("/a", "/a/b")), "T", "w2 below")
    eq(b(path.is_within("/a", "/ab")), "F", "w3 comp")
    eq(b(path.is_within("/a", "/a/../b")), "F", "w4 normd")
    eq(b(path.is_within("/workspace/foo", "/workspace/foobar")), "F", "w5 comp2")
    eq(b(path.is_within("/workspace", "/workspace/a/../../etc")), "F", "w6 escape-normd")
    eq(b(path.is_within("a", "a/b")), "T", "w7 rel")
    eq(b(path.is_within("a", "../a")), "F", "w8 rel-up")
    -- is_within: a relative candidate with MORE leading `..` than base escapes
    eq(b(path.is_within(".", "../x")), "F", "w9 dot-escape")
    eq(b(path.is_within("", "../../x")), "F", "w10 empty-escape")
    eq(b(path.is_within(".", "x")), "T", "w11 dot-child")
    eq(b(path.is_within("..", "../x")), "T", "w12 dotdot-child")
    -- invariants
    for _, x in ipairs({ "foo/./bar", "/a/../../b", "../../foo", "foo//bar/", "./x", "/a/b/../c" }) do
        eq(path.normalize(path.normalize(x)), path.normalize(x), "idem " .. x)
    end
    for _, pr in ipairs({ { "/a/b", "/a/c/x" }, { "src", "src/p/f.lua" }, { "/a", "/a/b/c" } }) do
        eq(path.normalize(path.join(pr[1], path.relative(pr[1], pr[2]))), path.normalize(pr[2]), "cmp")
    end
    -- type error is raised, not coerced
    eq(b(pcall(function() return path.normalize(42) end)), "F", "typeerr")

    if #fails > 0 then
        ctx.stderr:write("LUA FAIL:\n" .. table.concat(fails, "\n") .. "\n"); return 1
    end

    -- COMPREHENSIVE fingerprint: every op over a shared matrix.
    local FP_PATHS = { "a/b/../c", "/x//y/./z/..", "../p/q", "f.tar.gz", "/", ".gitignore", "foo/", "/a/../../b", "" }
    local FP_JOIN = { { "a", "/b" }, { "/a", "/b" }, { "foo/", "bar" }, { "src", "p", "f.lua" }, { "x", ".", "y" } }
    local FP_REL = { { "/a/b", "/a/c/x" }, { "src", "src/p/f" }, { "/a", "/a" } }
    local FP_WITHIN = { { ".", "../x" }, { "..", "../x" }, { "/a", "/a/b" }, { "/a", "/ab" } }
    local fp = {}
    for _, x in ipairs(FP_PATHS) do
        fp[#fp + 1] = table.concat({ path.normalize(x), path.basename(x), path.dirname(x),
            path.extension(x), path.stem(x), b(path.is_absolute(x)) }, ":")
    end
    for _, j in ipairs(FP_JOIN) do fp[#fp + 1] = path.join(table.unpack(j)) end
    for _, r in ipairs(FP_REL) do fp[#fp + 1] = path.relative(r[1], r[2]) end
    for _, w in ipairs(FP_WITHIN) do fp[#fp + 1] = b(path.is_within(w[1], w[2])) end
    ctx.stdout:write(table.concat(fp, "|") .. "\n"); return 0
end)
LUA

# ── JS app (identical semantics; camelCase for the two multi-word names) ───────
cat > "$WD/p.js" <<'JS'
import { app } from "hull:app";
import { path } from "hull:path";
app.manifest({ modules: ["hull/path@1"] });

app.main((ctx) => {
    const fails = [];
    const eq = (got, want, label) => { if (got !== want) fails.push(`${label}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); };
    const b = (v) => v ? "T" : "F";

    eq(path.normalize("foo//bar"), "foo/bar", "n1");
    eq(path.normalize("foo/./bar"), "foo/bar", "n2");
    eq(path.normalize("foo/a/../bar"), "foo/bar", "n3");
    eq(path.normalize("/a/../../b"), "/b", "n4");
    eq(path.normalize("../../foo"), "../../foo", "n5");
    eq(path.normalize("/"), "/", "n6");
    eq(path.normalize("."), ".", "n7");
    eq(path.normalize(""), ".", "n8");
    eq(path.normalize("foo/"), "foo", "n9");
    eq(path.normalize("/foo/"), "/foo", "n10");
    eq(path.normalize("foo/.."), ".", "n11");
    eq(path.normalize(".."), "..", "n12");
    eq(path.normalize("/foo/../bar"), "/bar", "n13");
    eq(path.join("foo", "bar"), "foo/bar", "j1");
    eq(path.join("foo/", "bar"), "foo/bar", "j2");
    eq(path.join("foo", ".", "bar"), "foo/bar", "j3");
    eq(path.join("src", "plugins", "foo.lua"), "src/plugins/foo.lua", "j4");
    eq(path.join(), ".", "j5");
    eq(path.join("", "foo", ""), "foo", "j6");
    eq(path.join("a", "/b"), "/b", "j7");
    eq(path.join("/a", "/b"), "/b", "j8");
    eq(path.join("a", "b/", "/c/d"), "/c/d", "j9");
    eq(path.dirname("/foo/bar.txt"), "/foo", "d1");
    eq(path.dirname("/foo"), "/", "d2");
    eq(path.dirname("foo"), ".", "d3");
    eq(path.dirname("foo/bar"), "foo", "d4");
    eq(path.dirname("/"), "/", "d5");
    eq(path.basename("/foo/bar.txt"), "bar.txt", "b1");
    eq(path.basename("/foo/bar/"), "bar", "b2");
    eq(path.basename("foo"), "foo", "b3");
    eq(path.basename("/"), "/", "b4");
    eq(path.extension("foo.txt"), ".txt", "e1");
    eq(path.extension("foo.tar.gz"), ".gz", "e2");
    eq(path.extension("foo"), "", "e3");
    eq(path.extension(".gitignore"), "", "e4");
    eq(path.extension("a.b.c"), ".c", "e5");
    eq(path.stem("/foo/archive.tar.gz"), "archive.tar", "s1");
    eq(path.stem(".gitignore"), ".gitignore", "s2");
    eq(path.stem("foo"), "foo", "s3");
    eq(b(path.isAbsolute("/foo/bar")), "T", "a1");
    eq(b(path.isAbsolute("foo")), "F", "a2");
    eq(path.relative("/a/b", "/a/c/file.txt"), "../c/file.txt", "r1");
    eq(path.relative("/a", "/a"), ".", "r2");
    eq(path.relative("src", "src/plugins/foo.lua"), "plugins/foo.lua", "r3");
    eq(b(path.isWithin("/a", "/a")), "T", "w1");
    eq(b(path.isWithin("/a", "/a/b")), "T", "w2");
    eq(b(path.isWithin("/a", "/ab")), "F", "w3");
    eq(b(path.isWithin("/a", "/a/../b")), "F", "w4");
    eq(b(path.isWithin("/workspace/foo", "/workspace/foobar")), "F", "w5");
    eq(b(path.isWithin("/workspace", "/workspace/a/../../etc")), "F", "w6");
    eq(b(path.isWithin("a", "a/b")), "T", "w7");
    eq(b(path.isWithin("a", "../a")), "F", "w8");
    eq(b(path.isWithin(".", "../x")), "F", "w9");
    eq(b(path.isWithin("", "../../x")), "F", "w10");
    eq(b(path.isWithin(".", "x")), "T", "w11");
    eq(b(path.isWithin("..", "../x")), "T", "w12");
    for (const x of ["foo/./bar", "/a/../../b", "../../foo", "foo//bar/", "./x", "/a/b/../c"]) {
        eq(path.normalize(path.normalize(x)), path.normalize(x), "idem " + x);
    }
    for (const [a, bb] of [["/a/b", "/a/c/x"], ["src", "src/p/f.lua"], ["/a", "/a/b/c"]]) {
        eq(path.normalize(path.join(a, path.relative(a, bb))), path.normalize(bb), "cmp");
    }
    let threw = false;
    try { path.normalize(42); } catch (_) { threw = true; }
    eq(b(threw), "T", "typeerr");

    if (fails.length > 0) { ctx.stderr.write("JS FAIL:\n" + fails.join("\n") + "\n"); return 1; }

    const FP_PATHS = ["a/b/../c", "/x//y/./z/..", "../p/q", "f.tar.gz", "/", ".gitignore", "foo/", "/a/../../b", ""];
    const FP_JOIN = [["a", "/b"], ["/a", "/b"], ["foo/", "bar"], ["src", "p", "f.lua"], ["x", ".", "y"]];
    const FP_REL = [["/a/b", "/a/c/x"], ["src", "src/p/f"], ["/a", "/a"]];
    const FP_WITHIN = [[".", "../x"], ["..", "../x"], ["/a", "/a/b"], ["/a", "/ab"]];
    const fp = [];
    for (const x of FP_PATHS) {
        fp.push([path.normalize(x), path.basename(x), path.dirname(x),
            path.extension(x), path.stem(x), b(path.isAbsolute(x))].join(":"));
    }
    for (const j of FP_JOIN) fp.push(path.join(...j));
    for (const r of FP_REL) fp.push(path.relative(r[0], r[1]));
    for (const w of FP_WITHIN) fp.push(b(path.isWithin(w[0], w[1])));
    ctx.stdout.write(fp.join("|") + "\n"); return 0;
});
JS

echo "== hull.path: Lua self-check + comprehensive fingerprint =="
LUA_OUT=$("$HULL" "$WD/p.lua") || { echo "LUA app FAILED"; exit 1; }
echo "  lua fp: $LUA_OUT"
echo "== hull.path: JS self-check + comprehensive fingerprint =="
JS_OUT=$("$HULL" "$WD/p.js") || { echo "JS app FAILED"; exit 1; }
echo "  js  fp: $JS_OUT"

if [ "$LUA_OUT" != "$JS_OUT" ]; then
    echo "PARITY FAIL: Lua and JS hull.path fingerprints differ"
    exit 1
fi
# golden: the exact lexical algorithm over every op (see the comment above).
EXPECT="a/c:c:a::c:F|/x/y:y:/x::y:T|../p/q:q:../p::q:F|f.tar.gz:f.tar.gz:.:.gz:f.tar:F|/:/:/::/:T|.gitignore:.gitignore:.::.gitignore:F|foo:foo:.::foo:F|/b:b:/::b:T|.:.:.::.:F|/b|/b|foo/bar|src/p/f.lua|x/y|../c/x|p/f|.|F|T|T|F"
if [ "$LUA_OUT" != "$EXPECT" ]; then
    echo "GOLDEN FAIL: fingerprint != expected"
    echo "  got:  $LUA_OUT"
    echo "  want: $EXPECT"
    exit 1
fi
echo "hull.path parity + semantics OK (Lua == JS == golden; all 9 ops in the fingerprint)"
