--- Pure LEXICAL path-name manipulation (no filesystem authority).
--
-- @module hull.path
-- @license AGPL-3.0-or-later
--
-- `hull.path` manipulates path NAMES lexically. `hull.fs` exercises filesystem
-- authority. These are separate on purpose.
--
-- SECURITY BOUNDARY (do not blur it):
--   `hull.path` performs lexical path manipulation ONLY. Lexical normalization
--   or containment (`is_within`) MUST NOT be used to authorize filesystem
--   access. A lexically-contained path may still traverse a symlink that
--   resolves OUTSIDE the root. Capability-checked filesystem operations
--   (`hull.fs` / the capability layer) must enforce containment against the
--   RESOLVED filesystem object, including symlink/reparse-point handling, after
--   safe resolution. `hull.path` never touches the filesystem, never inspects
--   symlinks, never checks existence, and grants no authority.
--
-- Canonical separator is `/`. Paths are plain strings; there are no path
-- objects. Every operation is O(length) and deterministic.

local path = {}

-- ── internal: lexical split ──────────────────────────────────────────────
-- Split `p` into (segments, absolute) with `.`/`..`/empty collapsed lexically:
--   * empty and `.` segments are dropped;
--   * `..` pops a preceding REAL segment; for an ABSOLUTE path a `..` at the
--     root is discarded (clamp at root: `/a/../../b` -> `/b`); for a RELATIVE
--     path a leading `..` (nothing real to pop) is KEPT so it stays meaningful
--     (`../../foo` -> `../../foo`).
-- Returns `segs` (array of strings) and `absolute` (boolean). Pure string work.
local function split(p)
    local absolute = p:sub(1, 1) == "/"
    local segs = {}
    local n = 0
    -- gmatch("[^/]+") already skips empty segments (collapses `//`).
    for seg in p:gmatch("[^/]+") do
        if seg == ".." then
            if n > 0 and segs[n] ~= ".." then
                segs[n] = nil
                n = n - 1
            elseif not absolute then
                n = n + 1
                segs[n] = ".."
            end
            -- absolute + nothing to pop: discard (clamp at root)
        elseif seg ~= "." then      -- `.` falls through (dropped)
            n = n + 1
            segs[n] = seg
        end
    end
    return segs, absolute
end

-- Rebuild a canonical string from split() output.
local function build(segs, absolute)
    local body = table.concat(segs, "/")
    if absolute then
        return "/" .. body            -- "/" when body == ""
    end
    if body == "" then
        return "."                    -- relative-empty normalizes to "."
    end
    return body
end

local function checktype(v, name)
    if type(v) ~= "string" then
        error("hull.path." .. name .. ": expected string, got " .. type(v), 3)
    end
end

--- Lexically normalize a path (collapse `//`, `.`, and `..`; never touches disk).
-- @tparam string p
-- @treturn string
function path.normalize(p)
    checktype(p, "normalize")
    local segs, absolute = split(p)
    return build(segs, absolute)
end

--- Join components with `/` and normalize the result. Empty components are
--- ignored. If a component is absolute it resets the accumulated path.
-- @param ... string components
-- @treturn string
function path.join(...)
    local parts = { ... }
    local buf = {}
    for i = 1, select("#", ...) do
        local c = parts[i]
        checktype(c, "join")
        if c ~= "" then
            buf[#buf + 1] = c
        end
    end
    if #buf == 0 then
        return "."
    end
    return path.normalize(table.concat(buf, "/"))
end

--- Directory portion of a path (lexical). `foo` -> `.`, `/foo` -> `/`.
-- @tparam string p
-- @treturn string
function path.dirname(p)
    checktype(p, "dirname")
    local segs, absolute = split(p)
    if #segs <= 1 then
        return absolute and "/" or "."
    end
    segs[#segs] = nil
    return build(segs, absolute)
end

--- Final component of a path (lexical). `/` -> `/`, `.` -> `.`.
-- @tparam string p
-- @treturn string
function path.basename(p)
    checktype(p, "basename")
    local segs, absolute = split(p)
    if #segs == 0 then
        return absolute and "/" or "."
    end
    return segs[#segs]
end

--- Final extension including the leading dot, or `""` if none. A leading-dot
--- basename with no other dot (a dotfile, e.g. `.gitignore`) has NO extension.
--- `foo.tar.gz` -> `.gz`. Go/.NET/Java-style; no MIME inference.
-- @tparam string p
-- @treturn string
function path.extension(p)
    checktype(p, "extension")
    local base = path.basename(p)
    -- last dot that is not the first character of the basename
    local dot = nil
    for i = #base, 2, -1 do
        if base:sub(i, i) == "." then
            dot = i
            break
        end
    end
    if not dot then
        return ""
    end
    return base:sub(dot)
end

--- Basename without its final extension. `archive.tar.gz` -> `archive.tar`.
-- @tparam string p
-- @treturn string
function path.stem(p)
    checktype(p, "stem")
    local base = path.basename(p)
    local ext = path.extension(p)
    if ext == "" then
        return base
    end
    return base:sub(1, #base - #ext)
end

--- Whether `p` is absolute (lexically: a leading `/`). NOT a security check;
--- performs no filesystem access.
-- @tparam string p
-- @treturn boolean
function path.is_absolute(p)
    checktype(p, "is_absolute")
    return p:sub(1, 1) == "/"
end

--- Lexical relative path FROM `base` TO `target` (both normalized first, no
--- filesystem access). `relative("/a/b", "/a/c/x")` -> `"../c/x"`. Errors if the
--- paths are incompatible under the lexical model (one absolute + one relative,
--- or `base` has leading `..` segments not shared by `target`).
-- @tparam string base
-- @tparam string target
-- @treturn string
function path.relative(base, target)
    checktype(base, "relative")
    checktype(target, "relative")
    local bsegs, babs = split(base)
    local tsegs, tabs = split(target)
    if babs ~= tabs then
        error("hull.path.relative: cannot relate absolute and relative paths", 2)
    end
    -- common prefix length
    local i = 1
    while i <= #bsegs and i <= #tsegs and bsegs[i] == tsegs[i] do
        i = i + 1
    end
    -- any leftover `..` in base means we cannot express the relation lexically
    for j = i, #bsegs do
        if bsegs[j] == ".." then
            error("hull.path.relative: base escapes above a shared root", 2)
        end
    end
    local out = {}
    for _ = i, #bsegs do
        out[#out + 1] = ".."
    end
    for j = i, #tsegs do
        out[#out + 1] = tsegs[j]
    end
    if #out == 0 then
        return "."
    end
    return table.concat(out, "/")
end

--- Whether `candidate` is lexically equal to or below `base` (COMPONENT-aware,
--- after normalizing both). `is_within("/a", "/a")` -> true; `is_within("/a",
--- "/ab")` -> false.
--
-- NOT A SECURITY CHECK. Lexical containment is not filesystem authorization: a
-- contained path may still traverse a symlink resolving outside `base`. Use for
-- diagnostics / lexical validation only; enforce real containment in `hull.fs`
-- against the resolved object.
-- @tparam string base
-- @tparam string candidate
-- @treturn boolean
function path.is_within(base, candidate)
    checktype(base, "is_within")
    checktype(candidate, "is_within")
    local bsegs, babs = split(base)
    local csegs, cabs = split(candidate)
    if babs ~= cabs then
        return false
    end
    if #csegs < #bsegs then
        return false
    end
    for i = 1, #bsegs do
        if bsegs[i] ~= csegs[i] then
            return false
        end
    end
    return true
end

return path
