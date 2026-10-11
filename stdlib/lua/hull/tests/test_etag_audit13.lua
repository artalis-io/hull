local etag = require("hull.web.middleware.etag")
local pass, fail = 0, 0
local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name .. ": " .. tostring(err)) end
end
for _, helper in ipairs({ "json", "text", "html" }) do
    for _, method in ipairs({ "GET", "HEAD" }) do
        for _, status in ipairs({ 201, 301, 401, 404, 500, 200 }) do
            test(helper .. " " .. method .. " status " .. status, function()
                local res = { code = 200, sent = false }
                function res:status(code) self.code = code; return self end
                function res:header() return self end
                res[helper] = function(self) self.sent = true; return self end
                etag[helper]({ method = method, headers = { ["if-none-match"] = "*" } },
                             res, helper == "json" and { ok = true } or "body", status)
                assert(res.code == (status == 200 and 304 or status))
                assert(res.sent == (status ~= 200))
            end)
        end
    end
end
return { pass = pass, fail = fail }
