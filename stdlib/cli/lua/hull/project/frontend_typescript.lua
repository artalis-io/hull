--
-- hull.project.frontend_typescript - the Lua PROXY for the TypeScript frontend.
--
-- TypeScript is the JavaScript parser in its TypeScript mode (docs/typescript_design.md 3.1), so
-- this is hull.project.frontend_javascript with the mode set to "ts", plus the operations only
-- TypeScript has: type_erasure(unit) (3.2) and erased_source(unit) (4.1). The same C bridge and the same QuickJS tooling session
-- run it; this module never parses TypeScript or touches QuickJS.
--
-- Never raises: every entry returns data or a Diagnostic-shaped error.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")
local js   = require("hull.project.frontend_javascript")

local M = {}

M.capabilities = { "declarations", "annotations", "source_ranges", "scope", "semantics", "type_erasure" }

local function diag(code, message)
    return { severity = "error", code = code, message = message }
end

function M.analyze_source(session, path, src)
    return js.analyze_source(session, path, src, "ts")
end

M.declarations          = js.declarations
M.decl_name             = js.decl_name
M.decl_kind             = js.decl_kind
M.decl_range            = js.decl_range
M.decl_group_range      = js.decl_group_range
M.decl_annotations      = js.decl_annotations
M.decl_is_method        = js.decl_is_method
M.declaration_semantics = js.declaration_semantics
M.scope                 = js.scope

-- type_erasure(unit) -> ({ spans, refusals }, nil) | (nil, Diagnostic). `spans` are half-open
-- byte ranges { start, stop } of erasable type syntax, sorted and non-overlapping; `refusals` the
-- ts.unsupported diagnostics (the file does not run while there is one).
function M.type_erasure(unit)
    if type(unit) ~= "table" or type(unit._session) ~= "number" or type(unit._unit_id) ~= "number"
        or unit._lang ~= "ts" then
        return nil, diag("typescript.internal", "type_erasure: invalid TypeScript unit handle")
    end
    local raw = tool.frontend_type_erasure("javascript", unit._session, unit._unit_id)
    if type(raw) ~= "string" then return nil, diag("typescript.internal", "type_erasure: no bridge result") end
    local ok, data = pcall(json.decode, raw)
    if not ok or type(data) ~= "table" then
        return nil, diag("typescript.internal", "type_erasure: malformed bridge JSON")
    end
    if data.ok ~= true then return nil, data.error or diag("typescript.internal", "type_erasure failed") end
    return { spans = data.spans or {}, refusals = data.refusals or {} }, nil
end

-- erased_source(unit) -> ({ bytes?, refusals }, nil) | (nil, Diagnostic) (4.1): the erasure
-- applied, in the frontend session, to the bytes it analyzed - each span byte a space, CR and
-- LF kept - so positions are unchanged. With any refusal (a ts.unsupported construct, or any
-- other error the parse reported) `bytes` is absent.
function M.erased_source(unit)
    if type(unit) ~= "table" or type(unit._session) ~= "number" or type(unit._unit_id) ~= "number"
        or unit._lang ~= "ts" then
        return nil, diag("typescript.internal", "erased_source: invalid TypeScript unit handle")
    end
    local raw = tool.frontend_erased_source("javascript", unit._session, unit._unit_id)
    if type(raw) ~= "string" then return nil, diag("typescript.internal", "erased_source: no bridge result") end
    local ok, data = pcall(json.decode, raw)
    if not ok or type(data) ~= "table" then
        return nil, diag("typescript.internal", "erased_source: malformed bridge JSON")
    end
    if data.ok ~= true then return nil, data.error or diag("typescript.internal", "erased_source failed") end
    local refusals = type(data.refusals) == "table" and data.refusals or {}
    if #refusals > 0 then return { refusals = refusals }, nil end
    local bytes = type(data.bytes) == "string" and require("hull.encoding").base64.decode(data.bytes)
    if type(bytes) ~= "string" then
        return nil, diag("typescript.internal", "erased_source: malformed bytes")
    end
    return { bytes = bytes, refusals = {} }, nil
end

return M
