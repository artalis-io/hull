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
    local f = io.open(path, "rb")
    if not f then return nil end
    local data = f:read("*a")
    f:close()
    return data
end

local function main()
    local app_dir = arg[1] or "."
    local sig_path = app_dir .. "/hull.sig"

    local sig_data = read_file(sig_path)
    if not sig_data then
        io.stderr:write("hull verify: no hull.sig found in " .. app_dir .. "\n")
        os.exit(1)
    end

    local sig = json.decode(sig_data)
    if not sig or not sig.files or not sig.signature or not sig.public_key then
        io.stderr:write("hull verify: invalid hull.sig format\n")
        os.exit(1)
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
        io.stderr:write("Missing files:\n")
        for _, name in ipairs(missing) do
            io.stderr:write("  " .. name .. "\n")
        end
    end
    if #mismatches > 0 then
        io.stderr:write("Modified files:\n")
        for _, m in ipairs(mismatches) do
            io.stderr:write("  " .. m.name .. "\n")
            io.stderr:write("    expected: " .. m.expected .. "\n")
            io.stderr:write("    actual:   " .. m.actual .. "\n")
        end
    end

    -- Verify Ed25519 signature
    local payload = json.encode({
        files = sig.files,
        manifest = sig.manifest,
    })
    local ok = crypto.ed25519_verify(payload, sig.signature, sig.public_key)

    if not ok then
        io.stderr:write("hull verify: FAILED — signature is invalid\n")
        os.exit(1)
    end

    if #missing > 0 or #mismatches > 0 then
        io.stderr:write("hull verify: FAILED — files do not match signature\n")
        os.exit(1)
    end

    print("hull verify: OK — all files verified, signature valid")
end

main()
