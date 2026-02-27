--
-- hull.verify — Verify app signature
--
-- Usage: hull verify [app_dir]
--
-- Reads hull.sig, recomputes file hashes, and verifies Ed25519 signature.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")

local function read_file(path)
    return tool.read_file(path)
end

local function main()
    local app_dir = arg[1] or "."
    local sig_path = app_dir .. "/hull.sig"

    local sig_data = read_file(sig_path)
    if not sig_data then
        tool.stderr("hull verify: no hull.sig found in " .. app_dir .. "\n")
        tool.exit(1)
    end

    local sig = json.decode(sig_data)
    if not sig or not sig.files or not sig.signature or not sig.public_key then
        tool.stderr("hull verify: invalid hull.sig format\n")
        tool.exit(1)
    end

    -- Recompute file hashes
    local mismatches = {}
    local missing = {}
    for name, expected_hash in pairs(sig.files) do
        local path = app_dir .. "/" .. name
        local data = read_file(path)
        if not data then
            missing[#missing + 1] = name
        else
            local actual_hash = crypto.sha256(data)
            if actual_hash ~= expected_hash then
                mismatches[#mismatches + 1] = {
                    name = name,
                    expected = expected_hash,
                    actual = actual_hash,
                }
            end
        end
    end

    -- Report file issues
    if #missing > 0 then
        tool.stderr("Missing files:\n")
        for _, name in ipairs(missing) do
            tool.stderr("  " .. name .. "\n")
        end
    end
    if #mismatches > 0 then
        tool.stderr("Modified files:\n")
        for _, m in ipairs(mismatches) do
            tool.stderr("  " .. m.name .. "\n")
            tool.stderr("    expected: " .. m.expected .. "\n")
            tool.stderr("    actual:   " .. m.actual .. "\n")
        end
    end

    -- Verify Ed25519 signature
    local payload = json.encode({
        files = sig.files,
        manifest = sig.manifest,
    })
    local ok = crypto.ed25519_verify(payload, sig.signature, sig.public_key)

    if not ok then
        tool.stderr("hull verify: FAILED — signature is invalid\n")
        tool.exit(1)
    end

    if #missing > 0 or #mismatches > 0 then
        tool.stderr("hull verify: FAILED — files do not match signature\n")
        tool.exit(1)
    end

    print("hull verify: OK — all files verified, signature valid")
end

main()
