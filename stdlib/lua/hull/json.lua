-- hull.json: the vendored JSON codec, with the app VM's input ceilings.
--
-- json.decode(str [, opts]) refuses a document nested deeper than the
-- limits' json_max_depth or with an object of more than json_max_keys keys
-- (defaults 128 and 100000; the manifest's `limits` sets them, see
-- docs/app_api_reference.md). Hull cannot tell which string came from a
-- request body, so every decode in the app VM is held to them. A caller may
-- pass its own { max_depth = N, max_keys = N } for one decode.
--
-- Outside Hull (a vanilla Lua state) hull._limits does not exist and decode
-- runs with no ceiling, as the vendored codec always did.
local vjson = require('vendor.json')

local has_native, limits = pcall(function()
    return require("hull._limits")
end)
if not has_native or type(limits) ~= "table" then limits = nil end

local M = {}
for k, v in pairs(vjson) do M[k] = v end

function M.decode(str, opts)
    if type(opts) == "table" or not limits then
        return vjson.decode(str, opts)
    end
    local max_depth, max_keys = limits.json()
    return vjson.decode(str, { max_depth = max_depth, max_keys = max_keys })
end

return M
