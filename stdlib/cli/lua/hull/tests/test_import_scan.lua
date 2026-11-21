--
-- test_import_scan.lua - hull.import_scan, the require / import scanners
-- `hull modules analyze` and `hull build` share. hull build reads the
-- relative specifiers to refuse an app that loads a file it leaves out of
-- the binary (tests/, root tooling configs; hsctl gap 11). Pure Lua; run by
-- tests/hull/source/test_lua_source.c.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name) end
end
local function same(list, want)
    if #list ~= #want then return false end
    for i = 1, #want do if list[i] ~= want[i] then return false end end
    return true
end

local scan = require("hull.import_scan")

-- Lua: hull modules and relative requires, comments and strings skipped.
do
    local set, rel = scan.scan_lua([[
local db = require("hull.db")
local h = require("./../tests/helper")
local u = require "./lib/util"
-- require("./tests/commented")
local s = "require('./tests/in_string')"
local x = myrequire("./tests/not_require")
local y = require("lib/bare")
app.get("/", function() end)
]])
    ok(set["hull/db"] and set["hull/http-server"], "lua: hull modules and decorations")
    ok(same(rel, { "./../tests/helper", "./lib/util" }), "lua: relative requires only")
end

-- JS / TypeScript: every import form that loads a relative module.
do
    local set, rel = scan.scan_js([[
import { db } from "hull:db";
import cfg from "../package.json";
import * as h from './tests/helper.ts';
import "./side.js";
export { a } from "./lib/a.js";
const m = import("./dyn.js");
import type { T } from "./tests/types.ts";
export type { U } from "./tests/more.ts";
// import x from "./tests/commented.js";
const s = "import y from './tests/in_string.js'";
import z from "lodash";
]])
    ok(set["hull/db"], "js: hull module")
    ok(same(rel, { "../package.json", "./tests/helper.ts", "./side.js", "./lib/a.js", "./dyn.js" }),
       "js: relative imports, type-only ones erased")
end

-- The hull-module results are those hull modules analyze reported before.
do
    local set = scan.scan_js('import "hull:crypto";\nexport * from "hull:time";\n')
    ok(next(set) == nil, "js: side-effect / re-export hull imports are not counted as uses")
    local lset = scan.scan_lua('require("hull.web.middleware.session")\n')
    ok(lset["hull/web/middleware/session"], "lua: dotted hull name")
end

return { pass = pass, fail = fail }
