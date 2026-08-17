--
-- hull.source.annotations — the generic `---@` scanner + declaration attachment.
--
-- Turns any `---@name`, `---@name(args)`, or `---@name rest` LINE comment into a
-- structured annotation record and attaches contiguous runs of such comments to
-- the declaration they lead. Deliberately WHITELIST-FREE: `name` is whatever
-- follows `@`, so an app's own `---@derive`, `---@route`, `---@query` annotations
-- are captured with the same fidelity as any LuaCATS tag. Consumers give meaning
-- to names; this layer only records them, with exact byte ranges.
--
-- Annotation record (docs/lua_source_analysis_design.md §6):
--   { name = "param",            -- the identifier after `@`
--     args = "a, b" | nil,       -- raw text inside a trailing (...) group, or nil
--     text = "x f64" | nil,      -- trailing text after the name / (...), trimmed
--     raw  = "---@param x f64",  -- the full original comment text
--     range = <SourceRange> }    -- the comment's half-open byte range
--
-- Attachment rule: a declaration's annotations are the annotation comments in the
-- UNBROKEN run of LEADING comment lines directly above it (no blank line, no code
-- between the run and the declaration; trailing comments on a code line do NOT
-- attach). A blank line breaks the run. On the target node:
--   node.annotation_list  -- ordered array of every annotation in the run (top->down)
--   node.annotations      -- name -> FIRST annotation of that name (repeat tags like
--                            @param keep all copies in annotation_list)
--
-- Pure Lua, no Hull C dependency. NEVER raises: M.attach is best-effort and the
-- public parse() wraps it defensively.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- DECLARATION kinds that can carry leading annotations. The contract (docs §8)
-- attaches annotations to DECLARATIONS only: `local x = ...` / `local function` and
-- global `function name() ... end`. Every other statement (assignments, calls,
-- loops, conditionals, returns, break/goto/labels) does NOT carry annotations, so a
-- `---@` run above one attaches to the nearest following declaration instead, or to
-- nothing. A function_EXPR is a value, not a statement; the enclosing
-- local_declaration / function_declaration is the attach target.
local DECLARATION_KINDS = {
    local_declaration = true,
    function_declaration = true,
}

-- Parse one comment into an annotation record, or nil when it is not a `---@`
-- annotation. Only LINE comments qualify, and only with THREE-or-more leading
-- hyphens then `@` (Lua's `--@x` two-hyphen form is an ordinary comment, matching
-- the LuaCATS convention).
function M.parse_comment(comment)
    if type(comment) ~= "table" or comment.kind ~= "line" then return nil end
    local body = comment.text:match("^%-%-%-+%s*@%s*(.+)$")
    if not body then return nil end
    local name = body:match("^([%a_][%w_]*)")
    if not name then return nil end                       -- `@` with no identifier

    local after = body:sub(#name + 1):match("^%s*(.*)$")  -- left-trim the remainder
    local args, text
    local group = after:match("^(%b())")                  -- a balanced (...) right after the name
    if group then
        args = group:sub(2, -2)                           -- strip the outer parens
        text = after:sub(#group + 1):match("^%s*(.-)%s*$")
    else
        text = after:match("^(.-)%s*$")                   -- right-trim
    end
    if text == "" then text = nil end

    return { name = name, args = args, text = text, raw = comment.text, range = comment.range }
end

-- Attach annotation runs to declaration nodes on unit.ast. Idempotent-ish: it
-- overwrites annotation_list / annotations on the nodes it touches. Returns the
-- unit. Best-effort: a malformed unit (no ast / comments) is a silent no-op.
function M.attach(unit)
    if type(unit) ~= "table" then return unit end
    local ast, comments, src = unit.ast, unit.comments, unit.source
    if type(ast) ~= "table" or type(comments) ~= "table" or type(src) ~= "string" then
        return unit
    end
    local starts = unit._linestarts

    -- One pass over every comment: (1) tag annotation comments as kind
    -- "annotation" and hang the parsed record off the comment (so unit.comments
    -- reflects kind in {line, long, annotation} per the contract, leading or not);
    -- (2) index the LEADING comments (nothing but whitespace before them on their
    -- own line) by end-line for attachment. A trailing comment on a code line
    -- (`x = 1  -- note`) is still tagged but never attaches downward.
    local by_end = {}                                     -- end_line -> comment info
    for _, c in ipairs(comments) do
        if type(c) == "table" and c.range then
            local rec = M.parse_comment(c)                -- parse BEFORE re-tagging kind
            if rec then c.kind = "annotation"; c.annotation = rec end
            local sl = unit:position(c.range.start)
            local el = unit:position(c.range.stop - 1)
            local line_start = starts[sl] or 1
            local before = src:sub(line_start, c.range.start - 1)
            if before:match("^%s*$") then
                by_end[el] = { comment = c, start_line = sl, ann = rec }
            end
        end
    end

    -- Collect every DECLARATION node (any nesting), in source order.
    local decls = {}
    local function collect(node)
        if type(node) ~= "table" then return end
        if node.kind and DECLARATION_KINDS[node.kind] and node.range then
            decls[#decls + 1] = node
        end
        for k, v in pairs(node) do
            if k ~= "range" and type(v) == "table" then collect(v) end
        end
    end
    collect(ast)

    for _, stmt in ipairs(decls) do
        local sl = unit:position(stmt.range.start)
        -- Walk contiguous leading comment lines upward from the line above.
        local run = {}
        local target = sl - 1
        while by_end[target] do
            local info = by_end[target]
            run[#run + 1] = info
            target = info.start_line - 1
        end
        if #run > 0 then
            local list, byname = {}, {}
            for j = #run, 1, -1 do                        -- reverse: top -> down
                local a = run[j].ann
                if a then
                    list[#list + 1] = a
                    if byname[a.name] == nil then byname[a.name] = a end
                end
            end
            if #list > 0 then
                stmt.annotation_list = list
                stmt.annotations = byname
            end
        end
    end

    return unit
end

-- node.annotations[name] (the first annotation of that name), or nil.
function M.get(node, name)
    if type(node) ~= "table" or type(node.annotations) ~= "table" then return nil end
    return node.annotations[name]
end

return M
