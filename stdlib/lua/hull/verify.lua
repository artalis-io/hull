--
-- hull.verify — Verify app signature (dual-layer)
--
-- Usage: hull verify [options] [app_dir]
--   --platform-key <file|url>   Platform public key (default: hardcoded gethull.dev key)
--   --developer-key <file|url>  Developer public key (required for full verification)
--
-- Reads package.sig (or hull.sig for backwards compat), verifies platform
-- and app layer Ed25519 signatures, and checks file hashes.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")

-- Hardcoded gethull.dev platform public key (from keys/gethull.dev.pub)
local GETHULL_DEV_PLATFORM_KEY = "0000000000000000000000000000000000000000000000000000000000000000"

local function read_file(path)
    return tool.read_file(path)
end

local function read_key(source)
    if not source then return nil end

    -- URL fetch
    if source:sub(1, 8) == "https://" then
        local data = tool.spawn_read({"curl", "-sfL", source})
        if data then
            return data:match("^(%x+)")
        end
        return nil
    end

    -- File path
    local data = read_file(source)
    if data then
        return data:match("^(%x+)")
    end
    return nil
end

local function parse_args()
    local opts = {
        app_dir = ".",
        platform_key = nil,
        developer_key = nil,
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--platform-key" then
            i = i + 1
            opts.platform_key = arg[i]
        elseif a == "--developer-key" then
            i = i + 1
            opts.developer_key = arg[i]
        elseif a:sub(1, 1) ~= "-" then
            opts.app_dir = a
        end
        i = i + 1
    end

    return opts
end

local function main()
    local opts = parse_args()
    local app_dir = opts.app_dir
    local issues = 0

    -- Try package.sig first, fall back to hull.sig
    local sig_path = app_dir .. "/package.sig"
    local sig_data = read_file(sig_path)
    local is_legacy = false
    if not sig_data then
        sig_path = app_dir .. "/hull.sig"
        sig_data = read_file(sig_path)
        is_legacy = true
    end

    if not sig_data then
        tool.stderr("hull verify: no package.sig or hull.sig found in " .. app_dir .. "\n")
        tool.exit(1)
    end

    local sig = json.decode(sig_data)
    if not sig or not sig.files or not sig.signature or not sig.public_key then
        tool.stderr("hull verify: invalid signature format\n")
        tool.exit(1)
    end

    -- ── Platform layer verification ────────────────────────────────
    if sig.platform and sig.platform.signature and sig.platform.public_key then
        local platform_key_hex = read_key(opts.platform_key) or GETHULL_DEV_PLATFORM_KEY

        -- Check if platform key matches
        if sig.platform.public_key == platform_key_hex then
            -- Verify platform signature
            local plat_payload = json.encode(sig.platform.platforms)
            local plat_ok = crypto.ed25519_verify(plat_payload,
                sig.platform.signature, sig.platform.public_key)

            if plat_ok then
                print("Platform layer: VALID (signed by Hull Platform)")
            else
                tool.stderr("Platform layer: FAILED — signature invalid\n")
                issues = issues + 1
            end
        else
            tool.stderr("Platform layer: WARNING — key mismatch (expected " ..
                platform_key_hex:sub(1, 16) .. "..., got " ..
                sig.platform.public_key:sub(1, 16) .. "...)\n")
            issues = issues + 1
        end

        -- Show platform architectures
        if sig.platform.platforms then
            local archs = {}
            for arch, _ in pairs(sig.platform.platforms) do
                archs[#archs + 1] = arch
            end
            table.sort(archs)
            print("  Architectures: " .. table.concat(archs, ", "))
        end
    elseif not is_legacy then
        tool.stderr("Platform layer: MISSING\n")
        issues = issues + 1
    end

    -- ── App layer verification ─────────────────────────────────────

    -- Determine developer key
    local dev_key_hex = read_key(opts.developer_key)
    if dev_key_hex then
        if sig.public_key ~= dev_key_hex then
            tool.stderr("App layer: WARNING — developer key mismatch\n")
            issues = issues + 1
        end
    end

    -- Verify app signature
    local payload
    if sig.binary_hash then
        -- New package.sig format
        payload = json.encode({
            binary_hash = sig.binary_hash,
            build = sig.build,
            files = sig.files,
            manifest = sig.manifest,
            platform = sig.platform,
            trampoline_hash = sig.trampoline_hash,
        })
    else
        -- Legacy hull.sig format
        payload = json.encode({
            files = sig.files,
            manifest = sig.manifest,
        })
    end

    local ok = crypto.ed25519_verify(payload, sig.signature, sig.public_key)
    if not ok then
        tool.stderr("App layer: FAILED — signature is invalid\n")
        tool.exit(1)
    end

    -- Show build info
    if sig.build then
        print("  Built with: " .. (sig.build.cc_version or sig.build.cc or "unknown"))
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
        issues = issues + #missing
    end
    if #mismatches > 0 then
        tool.stderr("Modified files:\n")
        for _, m in ipairs(mismatches) do
            tool.stderr("  " .. m.name .. "\n")
            tool.stderr("    expected: " .. m.expected .. "\n")
            tool.stderr("    actual:   " .. m.actual .. "\n")
        end
        issues = issues + #mismatches
    end

    if issues > 0 then
        tool.stderr("hull verify: FAILED — " .. issues .. " issue(s) found\n")
        tool.exit(1)
    end

    print("App layer: VALID")
    print("")
    print("hull verify: OK — all checks passed")
end

main()
