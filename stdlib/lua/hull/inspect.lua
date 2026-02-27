--
-- hull.inspect — Inspect app signature and capabilities
--
-- Usage: hull inspect [app_dir]
--
-- Reads hull.sig, displays capabilities, signature status, and file list.
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
        tool.stderr("hull inspect: no hull.sig found in " .. app_dir .. "\n")
        tool.exit(1)
    end

    local sig = json.decode(sig_data)
    if not sig then
        tool.stderr("hull inspect: invalid hull.sig format\n")
        tool.exit(1)
    end

    -- Display version
    print("Hull Signature v" .. (sig.version or "?"))
    print("")

    -- Display manifest/capabilities
    if sig.manifest then
        print("Capabilities:")
        local m = sig.manifest
        if m.fs then
            if m.fs.read then
                print("  fs.read:  " .. table.concat(m.fs.read, ", "))
            end
            if m.fs.write then
                print("  fs.write: " .. table.concat(m.fs.write, ", "))
            end
        end
        if m.env and #m.env > 0 then
            print("  env:      " .. table.concat(m.env, ", "))
        end
        if m.hosts and #m.hosts > 0 then
            print("  hosts:    " .. table.concat(m.hosts, ", "))
        end
        print("")
    end

    -- Display files
    if sig.files then
        print("Files:")
        for name, hash in pairs(sig.files) do
            print("  " .. name .. "  " .. hash)
        end
        print("")
    end

    -- Signature status
    if sig.signature then
        print("Signature:  " .. string.sub(sig.signature, 1, 32) .. "...")
    end
    if sig.public_key then
        print("Public key: " .. sig.public_key)
    end

    -- Verify if we can
    if sig.signature and sig.public_key and sig.files and sig.manifest then
        local payload = json.encode({
            files = sig.files,
            manifest = sig.manifest,
        })
        local ok = crypto.ed25519_verify(payload, sig.signature, sig.public_key)
        print("")
        if ok then
            print("Status: VALID")
        else
            print("Status: INVALID — signature does not match")
        end
    end
end

main()
