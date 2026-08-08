--- Internal request helpers shared across the web stdlib.
--
-- @module hull.web._request
-- @license AGPL-3.0-or-later
--
-- Contributor-only (the `_` prefix): required by other stdlib modules, never
-- declared by apps. Centralizes request-derived values that several middleware
-- (session, audit-log, totp, auth-flows) each extracted by hand - subtly
-- differently (some capped the length, some did not; some named the trust flag
-- `trust_proxy`, one `trust_xff`). See docs/stdlib_style.md §4.

local M = {}

--- The client's source IP, honoring a `trust_proxy` policy.
--
-- With `trust_proxy` false (the safe default), returns the un-spoofable socket
-- peer (`req.remote_addr`). With `trust_proxy` true - only correct behind a
-- trusted reverse proxy that sets it - returns the FIRST address of the
-- `X-Forwarded-For` chain (the original client), whitespace-trimmed, falling
-- back to the socket peer. Capped at 64 chars (IPv6 with headroom) so a hostile
-- multi-kilobyte XFF header can't land in an indexed column or a rate key.
--
-- @tparam table req            request object
-- @tparam[opt=false] boolean trust_proxy
-- @treturn string|nil          the client IP, or nil if unavailable
function M.client_ip(req, trust_proxy)
    if not (req and req.headers) then return nil end
    local ip
    local xff = req.headers["x-forwarded-for"]
    if trust_proxy and type(xff) == "string" and xff ~= "" then
        local first = xff:match("^([^,]+)")
        if first then
            local trimmed = first:gsub("^%s+", ""):gsub("%s+$", "")
            if trimmed ~= "" then ip = trimmed end
        end
    end
    if not ip and type(req.remote_addr) == "string" and req.remote_addr ~= "" then
        ip = req.remote_addr
    end
    if type(ip) == "string" and #ip > 64 then
        ip = ip:sub(1, 64)
    end
    return ip
end

return M
