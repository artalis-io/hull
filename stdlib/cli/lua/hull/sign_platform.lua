--
-- hull.sign_platform — Sign platform libraries
--
-- Usage: hull sign-platform [--dir DIR] <key_prefix>
--
-- Scans for libhull_platform*.a files, computes hashes, reads canary hashes,
-- and produces platform.sig (Ed25519-signed JSON).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")

-- ── Argument parsing ─────────────────────────────────────────────────

local function parse_args()
    local opts = {
        dir = "build/",
        key_prefix = nil,
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--dir" then
            i = i + 1
            opts.dir = arg[i]
        elseif a:sub(1, 1) ~= "-" then
            opts.key_prefix = a
        end
        i = i + 1
    end

    if not opts.key_prefix then
        tool.stderr("Usage: hull sign-platform [--dir DIR] <key_prefix>\n")
        tool.stderr("\n")
        tool.stderr("  key_prefix   Ed25519 key pair (reads <prefix>.key and <prefix>.pub)\n")
        tool.stderr("  --dir DIR    Directory containing platform libraries (default: build/)\n")
        tool.exit(1)
    end

    -- Ensure trailing slash
    if opts.dir:sub(-1) ~= "/" then
        opts.dir = opts.dir .. "/"
    end

    return opts
end

-- ── File utilities ───────────────────────────────────────────────────

local function read_file(path)
    return tool.read_file(path)
end

local function write_file(path, data)
    return tool.write_file(path, data)
end

local function file_exists(path)
    return tool.file_exists(path)
end

-- ── Architecture detection ──────────────────────────────────────────

-- Detect architecture from platform_cc file or filename suffix
local function detect_arch(dir, filename)
    -- Check for architecture suffix in filename:
    --   libhull_platform.x86_64-cosmo.a → x86_64-cosmo
    --   libhull_platform.aarch64-cosmo.a → aarch64-cosmo
    local arch = filename:match("libhull_platform%.(.-)%.a$")
    if arch then return arch end

    -- Single platform build: detect from platform_cc
    local cc_data = read_file(dir .. "platform_cc")
    if cc_data then
        local cc = cc_data:match("^%s*(.-)%s*$")
        if cc:find("cosmocc") then
            -- Detect host architecture for cosmo builds
            local uname = tool.spawn_read({"uname", "-m"})
            if uname then
                uname = uname:match("^%s*(.-)%s*$")
                if uname == "x86_64" or uname == "amd64" then
                    return "x86_64-cosmo"
                elseif uname == "aarch64" or uname == "arm64" then
                    return "aarch64-cosmo"
                end
            end
            return "x86_64-cosmo" -- fallback
        end
    end

    return "native"
end

-- ── Main ────────────────────────────────────────────────────────────

local function main()
    local opts = parse_args()
    local dir = opts.dir

    -- Read signing key
    local key_file = opts.key_prefix .. ".key"
    local key_data = read_file(key_file)
    if not key_data then
        tool.stderr("hull sign-platform: cannot read key file: " .. key_file .. "\n")
        tool.exit(1)
    end
    local sk_hex = key_data:match("^(%x+)")
    if not sk_hex or #sk_hex ~= 128 then
        tool.stderr("hull sign-platform: invalid key file format\n")
        tool.exit(1)
    end

    -- Read public key
    local pk_file = opts.key_prefix .. ".pub"
    local pk_data = read_file(pk_file)
    if not pk_data then
        tool.stderr("hull sign-platform: cannot read public key: " .. pk_file .. "\n")
        tool.exit(1)
    end
    local pk_hex = pk_data:match("^(%x+)")
    if not pk_hex or #pk_hex ~= 64 then
        tool.stderr("hull sign-platform: invalid public key format\n")
        tool.exit(1)
    end

    -- Find platform library files.
    --
    -- Search order:
    --   1. The dir the user passed (defaults to "build/") — preserves
    --      the existing source-tree workflow where the .a is already
    --      sitting in build/ after `make platform`.
    --   2. The embedded blob from the running hull binary — extract to
    --      a tmpdir and treat it as a second search location. End-users
    --      with a `gethull.dev/install.sh`-installed release binary
    --      land here. platform.sig is still written to the user-
    --      specified dir (creating it if needed).
    local platforms = {}
    local found = false

    -- Helper closure: scan a directory for libhull_platform*.a and
    -- accumulate into `platforms`. Returns true if it found anything.
    local function scan_dir(scan_dir_path)
        local hit = false

        -- Single-arch build (libhull_platform.a)
        local single_lib = scan_dir_path .. "libhull_platform.a"
        if file_exists(single_lib) then
            local arch = detect_arch(scan_dir_path, "libhull_platform.a")
            local lib_data = read_file(single_lib)
            if lib_data then
                local lib_hash = crypto.sha256(lib_data)
                -- Read canary hash (informational; absent on extracted
                -- embedded blobs since the canary lives next to the .a
                -- in the source tree only).
                local canary_data = read_file(scan_dir_path .. "platform_canary_hash")
                local canary_hash = canary_data and canary_data:match("^(%x+)") or nil
                platforms[arch] = {
                    hash = lib_hash,
                    canary = canary_hash,
                }
                hit = true
            end
        end

        -- Multi-arch builds (libhull_platform.<arch>.a)
        local multi_libs = tool.find_files(scan_dir_path, "libhull_platform.*.a")
        if multi_libs then
            for _, path in ipairs(multi_libs) do
                local filename = path:match("([^/]+)$")
                local arch = filename:match("libhull_platform%.(.-)%.a$")
                if arch then
                    local lib_data = read_file(path)
                    if lib_data then
                        local lib_hash = crypto.sha256(lib_data)
                        local canary_path = scan_dir_path .. "platform_canary_hash." .. arch
                        local canary_data = read_file(canary_path)
                        if not canary_data then
                            canary_data = read_file(scan_dir_path .. "platform_canary_hash")
                        end
                        local canary_hash = canary_data and canary_data:match("^(%x+)") or nil
                        platforms[arch] = {
                            hash = lib_hash,
                            canary = canary_hash,
                        }
                        hit = true
                    end
                end
            end
        end

        return hit
    end

    -- 1) Try the user's --dir (defaults to "build/").
    found = scan_dir(dir)

    -- 2) No .a's in --dir? Fall back to the embedded blob.
    if not found then
        local extracted = tool.tmpdir()
        local extracted_ok = false
        if tool.extract_platform_cosmo then
            extracted_ok = tool.extract_platform_cosmo(extracted) or extracted_ok
        end
        if not extracted_ok and tool.extract_platform then
            extracted_ok = tool.extract_platform(extracted) or extracted_ok
        end
        if extracted_ok then
            print("hull sign-platform: no .a in " .. dir ..
                  ", using embedded platform from this hull binary")
            found = scan_dir(extracted .. "/")
            -- The user's dir might not exist yet (e.g. CWD has no
            -- build/). Make it so platform.sig can be written there.
            if found then tool.mkdir(dir) end
        end
    end

    if not found then
        tool.stderr("hull sign-platform: no platform libraries found\n")
        tool.stderr("  searched: " .. dir .. "\n")
        tool.stderr("  this hull binary has no embedded platform library either\n")
        tool.stderr("  install a release binary from https://gethull.dev,\n")
        tool.stderr("  or run `make platform` in a hull source tree.\n")
        tool.exit(1)
    end

    -- Sign: canonicalStringify(platforms)
    local payload = json.encode(platforms)
    local sig_hex = crypto.ed25519_sign(payload, sk_hex)

    -- Build platform.sig
    local sig_table = {
        platforms = platforms,
        signature = sig_hex,
        public_key = pk_hex,
    }

    local sig_json = json.encode(sig_table)
    local sig_path = dir .. "platform.sig"
    write_file(sig_path, sig_json .. "\n")

    -- Display summary
    print("hull sign-platform: signed platform libraries")
    for arch, info in pairs(platforms) do
        print("  " .. arch .. ": " .. info.hash:sub(1, 16) .. "...")
        if info.canary then
            print("    canary: " .. info.canary:sub(1, 16) .. "...")
        end
    end
    print("wrote " .. sig_path)
end

main()
