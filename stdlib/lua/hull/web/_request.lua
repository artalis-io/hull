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

local _text = require("hull._text")   -- linear trims (see hull._text)

local M = {}

--- The client's source IP, honoring a `trust_proxy` policy.
--
-- With `trust_proxy` false (the safe default), returns the un-spoofable socket
-- peer (`req.remote_addr`). With `trust_proxy` true - only correct behind a
-- trusted reverse proxy - returns the LAST address of the `X-Forwarded-For`
-- chain, whitespace-trimmed, falling back to the socket peer. Proxies append
-- the peer they saw; everything to its left came from the client and is
-- whatever the client wrote, so the first entry was spoofable (a fresh rate
-- limit bucket per request). Behind more than one proxy layer, put the outer
-- one in charge of the header (or strip it) so the last entry is the client. Capped at 64 chars (IPv6 with headroom) so a hostile
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
        -- Anchored: the unanchored "([^,]+)$" retried from every offset, which
        -- is quadratic in a long run with no comma (the header is the
        -- client's; 0.7 s of event loop for 8 KB). "^.*," backs off from the
        -- end once.
        local last = xff:match("^.*,(.*)$") or xff
        if last then
            local trimmed = _text.trim(last)
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


--- The key a per-client limit (a rate limit, a lockout) counts an address
-- under: an IPv4 address as itself, an IPv6 address as its /64. Anyone with
-- one IPv6 host holds a whole /64 and can take a fresh address per request,
-- so keyed by the full address every request had its own budget - the TOTP
-- per-IP gate and any login rate limit did nothing. IPv4-mapped addresses
-- (::ffff:a.b.c.d) count as the IPv4 address; anything that does not parse
-- is returned as it is.
--
-- @tparam string|nil ip
-- @treturn string|nil
function M.limit_key(ip)
    if type(ip) ~= "string" or ip == "" or not ip:find(":", 1, true) then
        return ip
    end
    ip = ip:gsub("%%.*$", "")                               -- zone ("fe80::1%eth0")
    local v4 = ip:match("^::[fF][fF][fF][fF]:(%d+%.%d+%.%d+%.%d+)$")
    if v4 then return v4 end
    local function split(s, out)
        if s == "" then return true end
        for g in (s .. ":"):gmatch("([^:]*):") do
            if not g:match("^%x%x?%x?%x?$") then return false end
            out[#out + 1] = g
        end
        return true
    end
    local groups = {}
    local head, tail = ip:match("^(.-)::(.*)$")
    if head then
        local h, t = {}, {}
        if not split(head, h) or not split(tail, t) or #h + #t > 7 then return ip end
        for _, g in ipairs(h) do groups[#groups + 1] = g end
        for _ = 1, 8 - #h - #t do groups[#groups + 1] = "0" end
        for _, g in ipairs(t) do groups[#groups + 1] = g end
    elseif not split(ip, groups) or #groups ~= 8 then
        return ip
    end
    for i = 1, 4 do groups[i] = string.format("%x", tonumber(groups[i], 16)) end
    return groups[1] .. ":" .. groups[2] .. ":" .. groups[3] .. ":" .. groups[4] .. "::/64"
end


--- A user id as the stdlib stores and compares it (the user_id columns are
-- text): a non-empty string as is, an integer as its decimal string. Apps
-- key users by INTEGER PRIMARY KEY as often as by text, and every stdlib
-- entry point that takes a user id goes through this, so 42 and "42" are
-- the same user everywhere - session, audit-log, totp, rbac. Anything else
-- (nil, "", a float, a table) is no id: nil.
--
-- @param user_id
-- @treturn string|nil
function M.user_id(user_id)
    if type(user_id) == "string" then
        if user_id == "" then return nil end
        return user_id
    end
    if math.type(user_id) == "integer" then return tostring(user_id) end
    return nil
end

return M
