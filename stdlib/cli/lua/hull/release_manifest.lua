--
-- hull.release_manifest - read one entry of a signed sha256 manifest.
--
-- The manifests `hull verify` checks (package.sig.platform.gethull.manifest,
-- a composed platform_domain manifest) are the ones the runtime reads with
-- hl_release_io_find_checksum (src/hull/release.c). This reads them the same
-- way, so `hull verify` reports VALID only what `--verify-sig` accepts: each
-- line is exactly "<64 hex>  <name>" ("\r\n" tolerated), and a name listed
-- twice, or listed with a malformed hash, has no entry (audit 18: the first
-- matching line was taken, with any whitespace and an optional '*').
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

--- The lower-case hex SHA-256 the manifest lists for `name`, or nil when it
--- lists none, lists it twice, or lists it with a hash that is not 64 hex.
function M.hash(manifest, name)
    if type(manifest) ~= "string" or type(name) ~= "string" or name == "" then
        return nil
    end
    local want = #name + 66
    local found = nil
    local pos = 1
    while pos <= #manifest do
        local eol = manifest:find("\n", pos, true)
        local line = manifest:sub(pos, (eol or (#manifest + 1)) - 1)
        if line:sub(-1) == "\r" then line = line:sub(1, -2) end
        if #line == want and line:sub(65, 66) == "  " and line:sub(67) == name then
            local h = line:sub(1, 64)
            if found or not h:match("^%x+$") then return nil end
            found = h:lower()
        end
        if not eol then break end
        pos = eol + 1
    end
    return found
end

return M
