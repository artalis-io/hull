--
-- hull.inspect — Inspect app signature and capabilities (dual-layer)
--
-- Usage: hull inspect [app_dir]
--
-- Reads package.sig (or hull.sig), displays capabilities, dual-layer
-- signature status, build info, and file list.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")

local function read_file(path)
    return tool.read_file(path)
end

local function main()
    local app_dir = arg[1] or "."

    -- Try package.sig first, fall back to hull.sig
    local sig_path = app_dir .. "/package.sig"
    local sig_data = read_file(sig_path)
    local format_name = "package.sig"
    if not sig_data then
        sig_path = app_dir .. "/hull.sig"
        sig_data = read_file(sig_path)
        format_name = "hull.sig (legacy)"
    end

    if not sig_data then
        tool.stderr("hull inspect: no package.sig or hull.sig found in " .. app_dir .. "\n")
        tool.exit(1)
    end

    local sig = json.decode(sig_data)
    if not sig then
        tool.stderr("hull inspect: invalid signature format\n")
        tool.exit(1)
    end

    -- Display format
    print("Hull Signature (" .. format_name .. ")")
    print("")

    -- ── Platform layer ─────────────────────────────────────────────
    if sig.platform then
        print("Platform Layer:")
        if sig.platform.public_key then
            print("  Key:       " .. sig.platform.public_key)
        end
        if sig.platform.signature then
            print("  Signature: " .. string.sub(sig.platform.signature, 1, 32) .. "...")
        end
        if sig.platform.platforms then
            local archs = {}
            for arch, info in pairs(sig.platform.platforms) do
                local desc = arch .. " (" .. (info.hash or "?"):sub(1, 12) .. "...)"
                if info.canary then
                    desc = desc .. " canary:" .. info.canary:sub(1, 12) .. "..."
                end
                archs[#archs + 1] = desc
            end
            table.sort(archs)
            print("  Platforms:")
            for _, a in ipairs(archs) do
                print("    " .. a)
            end
        end

        -- Verify platform signature if possible
        if sig.platform.signature and sig.platform.public_key and sig.platform.platforms then
            local plat_payload = json.encode(sig.platform.platforms)
            local plat_ok = crypto.ed25519_verify(plat_payload,
                sig.platform.signature, sig.platform.public_key)
            if plat_ok then
                print("  Status:    VALID")
            else
                print("  Status:    INVALID")
            end
        end
        print("")
    end

    -- ── App layer ──────────────────────────────────────────────────
    print("Application Layer:")

    if sig.public_key then
        print("  Key:       " .. sig.public_key)
    end
    if sig.signature then
        print("  Signature: " .. string.sub(sig.signature, 1, 32) .. "...")
    end

    -- Build info
    if sig.build then
        print("  Build:")
        if sig.build.cc then
            print("    CC:      " .. sig.build.cc)
        end
        if sig.build.cc_version then
            print("    Version: " .. sig.build.cc_version)
        end
        if sig.build.flags then
            print("    Flags:   " .. sig.build.flags)
        end
    end

    -- Binary hash
    if sig.binary_hash then
        print("  Binary:    " .. sig.binary_hash)
    end

    -- Trampoline hash
    if sig.trampoline_hash then
        print("  Trampoline:" .. sig.trampoline_hash)
    end

    -- Verify app signature
    if sig.signature and sig.public_key and sig.files then
        local payload
        if sig.binary_hash then
            payload = json.encode({
                binary_hash = sig.binary_hash,
                build = sig.build,
                files = sig.files,
                manifest = sig.manifest,
                platform = sig.platform,
                trampoline_hash = sig.trampoline_hash,
            })
        else
            payload = json.encode({
                files = sig.files,
                manifest = sig.manifest,
            })
        end
        local ok = crypto.ed25519_verify(payload, sig.signature, sig.public_key)
        if ok then
            print("  Status:    VALID")
        else
            print("  Status:    INVALID")
        end
    end
    print("")

    -- ── Capabilities ───────────────────────────────────────────────
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

    -- ── Files ──────────────────────────────────────────────────────
    if sig.files then
        local count = 0
        for _ in pairs(sig.files) do count = count + 1 end
        print("Files (" .. count .. "):")
        local names = {}
        for name in pairs(sig.files) do
            names[#names + 1] = name
        end
        table.sort(names)
        for _, name in ipairs(names) do
            print("  " .. name .. "  " .. sig.files[name])
        end
    end
end

main()
