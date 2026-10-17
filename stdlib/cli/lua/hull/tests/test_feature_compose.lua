--
-- test_feature_compose.lua - the signed-feature-cache tier (audit 17).
--
-- feature_compose.resolve_lib verified a cache-sourced archive in place and
-- returned the cache path; the caller copied it into the build directory
-- later, past the check, and plan_mandatory recorded it in the PLATFORM
-- attestation domain although the release-key manifest is what verified it.
-- Now, with a build directory, the archive is copied first and the copy is
-- verified and returned, and plan_mandatory records a cache-sourced archive
-- in the release domain under its release-asset name. Pure Lua; run by
-- tests/hull/source/test_lua_source.c with the tool VM stubbed.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name) end
end

local CACHE = "/home/u/.hull/feature"
local present, copies, verifies = {}, {}, {}
local verify_ok = true
tool = {
    file_exists = function(p) return present[p] == true end,
    feature_cache_dir = function() return CACHE end,
    copy = function(src, dst)
        copies[#copies + 1] = { src, dst }
        present[dst] = true
        return true
    end,
    platform_verify = function(dir, asset, file)
        verifies[#verifies + 1] = { dir, asset, file }
        return verify_ok
    end,
    modules_resolve = function()
        return { ok = true, needs_http = false }
    end,
}

local fc = require("hull.feature_compose")
fc.extract_manifest_or_fatal = function() return {} end

local ASSET = "libhull_feature-lua-linux-x86_64.a"
present[CACHE .. "/" .. ASSET] = true

-- ── with a build directory: copy first, verify the copy ──────────────
do
    copies, verifies = {}, {}
    local p, from = fc.resolve_lib("libhull_feature-lua.a", ASSET,
        { hull_dir = "", plat = "linux-x86_64", tmpdir = "/tmp/b" })
    ok(p == "/tmp/b/libhull_feature-lua.a", "the copy is returned")
    ok(from == "cache", "from the cache")
    ok(#copies == 1 and copies[1][1] == CACHE .. "/" .. ASSET, "copied from the cache")
    ok(#verifies == 1 and verifies[1][3] == "/tmp/b/libhull_feature-lua.a",
       "the copy is what is verified")
end

-- ── a copy that does not verify is refused ───────────────────────────
do
    copies, verifies = {}, {}
    verify_ok = false
    local p, why = fc.resolve_lib("libhull_feature-lua.a", ASSET,
        { hull_dir = "", plat = "linux-x86_64", tmpdir = "/tmp/b" })
    ok(p == nil and why == "cache-verify-failed", "an unverified copy is refused")
    verify_ok = true
end

-- ── no build directory: the cache file itself is verified ────────────
do
    copies, verifies = {}, {}
    local p = fc.resolve_lib("libhull_feature-lua.a", ASSET,
        { hull_dir = "", plat = "linux-x86_64" })
    ok(p == CACHE .. "/" .. ASSET, "the cache path without a build dir")
    ok(#copies == 0 and #verifies == 1 and verifies[1][3] == nil, "verified in place")
end

-- ── plan_mandatory records a cache-sourced runtime in the release domain ──
do
    local plan = fc.plan_mandatory({
        app_dir = "/app", app_rt = "lua", tmpdir = "/tmp/b", hull_dir = "",
        plat = "linux-x86_64", platform_lib = "/tmp/b/libhull_platform.a",
        on_fail = function(msg) error(msg) end,
    })
    local d = plan.list[1]
    ok(d and d.path == "/tmp/b/libhull_feature-lua.a", "the plan links the verified copy")
    ok(d and d.domain == "release", "a cache-sourced archive is in the release domain")
    ok(d and d.record_name == ASSET, "under its release-asset name")
end

-- ── an embedded runtime stays in the platform domain ─────────────────
do
    tool.extract_feature_runtime = function(dir, rt)
        present[dir .. "/libhull_feature-" .. rt .. ".a"] = true
        return true
    end
    local plan = fc.plan_mandatory({
        app_dir = "/app", app_rt = "lua", tmpdir = "/tmp/c", hull_dir = "",
        plat = "linux-x86_64", platform_lib = "/tmp/c/libhull_platform.a",
        on_fail = function(msg) error(msg) end,
    })
    local d = plan.list[1]
    ok(d and d.domain == "platform", "an embedded archive is in the platform domain")
    ok(d and d.record_name == "libhull_feature-lua.linux-x86_64.a",
       "under its embedded-asset name")
end

return { pass = pass, fail = fail }
