--
-- hull.source.discover - the canonical, hardened, bounded source-file walker.
--
-- Extracted from hull.source.analyze so `hull analyze` and the project analyzer
-- (hull.project.*) share ONE discovery path (design: docs/project_discovery_design.md
-- D2). No second recursive walker exists in Hull. The behavior is exactly what
-- `hull analyze` shipped: exclude dirs pruned DURING traversal, canonical containment
-- via tool.realpath, deterministic sorted/regular/no-symlink results, fail-closed on a
-- discovery error (returned as (nil, err) -- the CALLER decides policy: `hull analyze`
-- op_fails, the project analyzer records a diagnostic).
--
-- Pure Lua over the tool VM's tool.* bindings (find_files/realpath/path_kind). Never
-- raises, never prints.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- Generated / dependency dirs pruned by default (build covers site/build). The project
-- analyzer adds "static" via opts.extra_exclude so browser assets never count as
-- application source (design D6).
M.DEFAULT_EXCLUDE = { ".git", ".hull", "build", "vendor", "node_modules" }

-- ── small path helpers (pure Lua; no path-normalize binding in the tool VM) ──
-- Collapse ./ and ../ segments. Absolute paths keep a leading "/"; a ".." at an
-- absolute root stays at root.
function M.normalize(p)
    local absolute = p:sub(1, 1) == "/"
    local segs = {}
    for seg in p:gmatch("[^/]+") do
        if seg == ".." then
            if #segs > 0 and segs[#segs] ~= ".." then table.remove(segs)
            elseif not absolute then segs[#segs + 1] = ".." end   -- absolute: .. at root stays root
        elseif seg ~= "." then                               -- "." is dropped
            segs[#segs + 1] = seg
        end
    end
    return (absolute and "/" or "") .. table.concat(segs, "/")
end

function M.join(root, rel)
    if rel:sub(1, 1) == "/" then return rel end              -- absolute stays absolute
    if root == "" or root == "." then return rel end
    return root .. "/" .. rel
end

-- Containment on CANONICAL absolute paths (from tool.realpath, symlinks resolved).
-- Handles the `/` root correctly (prefix is "/", not "//").
function M.inside_root(canon_root, canon_target)
    if canon_root == canon_target then return true end
    local prefix = (canon_root == "/") and "/" or (canon_root .. "/")
    return canon_target:sub(1, #prefix) == prefix
end

-- Build the { seg -> true } exclusion set for a run.
local function exclude_set(extra)
    local set = {}
    for _, s in ipairs(M.DEFAULT_EXCLUDE) do set[s] = true end
    for _, s in ipairs(extra or {}) do set[s] = true end
    return set
end

-- A path is excluded if ANY of its segments is an excluded dir name (belt over the
-- returned paths, complementing exclude_dirs pruning during traversal).
local function excluded(path, set)
    for seg in path:gmatch("[^/]+") do
        if set[seg] then return true end
    end
    return false
end

-- ── discovery (walk mode): sorted, regular, no symlink, exclusions ──
-- discover(root, opts?) -> (files[], err)
--   opts.ext           : array of extensions WITHOUT the dot (default {"lua"}). One
--                        find_files glob per extension; results merged + deduped.
--   opts.extra_exclude : extra dir names pruned on top of DEFAULT_EXCLUDE (e.g.
--                        {"static"} for the project analyzer's application-source scan).
-- exclude_dirs prunes DURING traversal (a large build/ tree is never walked); find_files
-- already returns sorted/regular/no-symlink; excluded() is the belt. A discovery error
-- (OOM / access) is returned as (nil, err) -- an OPERATIONAL failure, never an empty scan.
function M.discover(root, opts)
    opts = opts or {}
    local exts = opts.ext or { "lua" }
    local exclude_list = {}
    for _, s in ipairs(M.DEFAULT_EXCLUDE) do exclude_list[#exclude_list + 1] = s end
    for _, s in ipairs(opts.extra_exclude or {}) do exclude_list[#exclude_list + 1] = s end
    local set = exclude_set(opts.extra_exclude)

    local out, seen = {}, {}
    for _, ext in ipairs(exts) do
        local files, err = tool.find_files(root, "*." .. ext, { exclude_dirs = exclude_list })
        if err then return nil, err end                      -- fail closed; caller decides
        for _, p in ipairs(files) do
            local n = M.normalize(p)
            if not excluded(n, set) and not seen[n] then
                seen[n] = true; out[#out + 1] = n
            end
        end
    end
    table.sort(out)
    return out
end

return M
