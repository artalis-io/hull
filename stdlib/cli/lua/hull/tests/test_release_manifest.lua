--
-- test_release_manifest.lua - hull verify's manifest reader (audit 18).
--
-- release_manifest.hash must read a signed sha256 manifest exactly as the
-- runtime's hl_release_io_find_checksum does: "<64 hex>  <name>" lines,
-- "\r\n" tolerated, and a name listed twice (or with a malformed hash) has no
-- entry. verify.lua took the first matching line, with any whitespace and an
-- optional '*', so it could report VALID what --verify-sig refuses. Pure Lua;
-- run by tests/hull/source/test_lua_source.c.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name) end
end

local rm = require("hull.release_manifest")

local A = string.rep("a", 64)
local B = string.rep("b", 64)
local U = string.rep("C", 64)

ok(rm.hash(A .. "  x86_64\n" .. B .. "  aarch64\n", "x86_64") == A, "plain entry")
ok(rm.hash(A .. "  x86_64\n" .. B .. "  aarch64", "aarch64") == B, "last line, no newline")
ok(rm.hash(A .. "  x86_64\r\n", "x86_64") == A, "CRLF tolerated")
ok(rm.hash(U .. "  x86_64\n", "x86_64") == string.rep("c", 64), "lower-cased")
ok(rm.hash(A .. "  x86_64\n", "aarch64") == nil, "absent name")

-- A duplicate name has no entry, whichever line comes first.
ok(rm.hash(A .. "  x86_64\n" .. B .. "  x86_64\n", "x86_64") == nil, "duplicate refused")
ok(rm.hash(A .. "  x86_64\n" .. A .. "  x86_64\n", "x86_64") == nil, "identical duplicate refused")

-- The C format exactly: two spaces, no '*', no trailing text or whitespace.
ok(rm.hash(A .. " x86_64\n", "x86_64") == nil, "one space")
ok(rm.hash(A .. "  *x86_64\n", "x86_64") == nil, "binary-mode '*'")
ok(rm.hash(A .. "\tx86_64\n", "x86_64") == nil, "tab")
ok(rm.hash(A .. "  x86_64 \n", "x86_64") == nil, "trailing space")
ok(rm.hash(A .. "  x86_64\0\n", "x86_64") == nil, "trailing NUL")
ok(rm.hash(string.rep("a", 63) .. "g  x86_64\n", "x86_64") == nil, "non-hex hash")
ok(rm.hash(string.rep("a", 63) .. "  x86_64\n", "x86_64") == nil, "short hash")
-- A malformed line for the name makes the whole lookup fail, as in C.
ok(rm.hash(string.rep("z", 64) .. "  x86_64\n" .. A .. "  x86_64\n", "x86_64") == nil,
   "malformed line for the name refused")

ok(rm.hash(nil, "x") == nil and rm.hash(A .. "  x\n", nil) == nil and
   rm.hash(A .. "  \n", "") == nil, "bad arguments")

return { pass = pass, fail = fail }
