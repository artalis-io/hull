--
-- hull.inspect - Inspect app signature and capabilities (dual-layer)
--
-- Usage: hull inspect [app_dir]
--
-- Reads package.sig (or hull.sig), displays capabilities, dual-layer
-- signature status, build info, and file list.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")
local hex = require("hull.encoding").hex

-- Verify a hex ed25519 signature over `payload` with a hex public key, as
-- stored in package.sig / platform.sig. Malformed hex is a failed check.
local function ed25519_verify_hex(payload, sig_hex, pk_hex)
    local sig = type(sig_hex) == "string" and hex.decode(sig_hex)
    local pk  = type(pk_hex) == "string" and hex.decode(pk_hex)
    if not sig or not pk or #sig ~= 64 or #pk ~= 32 then return false end
    return crypto.ed25519_verify(payload, sig, pk)
end

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
            local plat_ok = ed25519_verify_hex(plat_payload,
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
        -- The exact payload build.lua signed (hull.app_sig, shared with it).
        local payload = require("hull.app_sig").payload(sig)
        local ok = ed25519_verify_hex(payload, sig.signature, sig.public_key)
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
        -- W^X escape hatches - surface loudly so the auditor sees them.
        -- Accept both snake_case (Lua manifest) and camelCase (JS manifest).
        local adc = m.allow_dynamic_code or m.allowDynamicCode
        local adl = m.allow_dynamic_libraries or m.allowDynamicLibraries
        if adc then
            print("  [!] allow_dynamic_code:      true  (RISKY - JIT / runtime codegen)")
        end
        if adl then
            print("  [!] allow_dynamic_libraries: true  (RISKY - dlopen of native libs)")
        end
        print("")
        -- Declared commands (docs/design/exec_broker.md §3): what each may run,
        -- with which arguments, and the authority unix_sockets carries.
        if type(m.exec) == "table" and next(m.exec) then
            print("Commands (exec):")
            local names = {}
            for name in pairs(m.exec) do names[#names + 1] = name end
            table.sort(names)
            local function slot_desc(s)
                if type(s.enum) == "table" then
                    local e = {}
                    for _, v in ipairs(s.enum) do e[#e + 1] = tostring(v) end
                    return "<one of: " .. table.concat(e, ", ") .. ">"
                end
                local parts = {}
                if s.prefix then parts[#parts + 1] = "prefix " .. string.format("%q", tostring(s.prefix)) end
                if s.suffix then parts[#parts + 1] = "suffix " .. string.format("%q", tostring(s.suffix)) end
                parts[#parts + 1] = "chars " .. tostring(s.chars or "A-Za-z0-9._-")
                parts[#parts + 1] = "len " .. tostring(s.min or 1) .. ".." .. tostring(s.max or 256)
                local rep = s["repeat"]
                if type(rep) == "table" then
                    parts[#parts + 1] = "x" .. tostring(rep.min or 1) .. ".." .. tostring(rep.max or "?")
                end
                if s.allow_dash or s.allowDash then parts[#parts + 1] = "[!] may start with '-'" end
                return "<" .. table.concat(parts, ", ") .. ">"
            end
            -- Manifest text is a cloned tree's: one line each (audit 53).
            local function esc(v) return tool.escape(tostring(v)) end
            -- Only the strings a list holds (the extractor refuses the rest).
            local function strs(l)
                local out = {}
                for _, v in ipairs(type(l) == "table" and l or {}) do
                    if type(v) == "string" then out[#out + 1] = esc(v) end
                end
                return out
            end
            local function list(l)
                local t = strs(l)
                return #t > 0 and table.concat(t, ", ") or nil
            end
            -- The host grants as the extractor takes them (audit 53).
            local function host_of(c)
                return type(c.host) == "table" and c.host or {}
            end
            local function unix_of(c)
                return type(c.host) == "table" and (c.host.unix_sockets or c.host.unixSockets)
            end
            for _, name in ipairs(names) do
                local c = m.exec[name]
                if type(c) ~= "table" then c = {} end
                print("  " .. esc(name) .. "  " .. esc(c.path))
                if type(c.sha256) == "string" then
                    print("    sha256 pin: " .. esc(c.sha256:lower()))
                end
                local argv = {}
                for _, a in ipairs(type(c.argv) == "table" and c.argv or {}) do
                    argv[#argv + 1] = type(a) == "table" and esc(slot_desc(a))
                                      or esc(string.format("%q", tostring(a)))
                end
                if #argv > 0 then print("    argv: " .. table.concat(argv, " ")) end
                local function num(v, d) return math.tointeger(v) or d end
                local t = num(c.timeout_ms or c.timeoutMs, 5000)
                local so = num(c.stdout_max or c.stdoutMax, 65536)
                local se = num(c.stderr_max or c.stderrMax, 16384)
                print(string.format("    limits: %d ms, stdout %d, stderr %d, %d at once",
                                    t, so, se, num(c.concurrency, 2)))
                -- Its whole authority: the host grants (absolute paths) and
                -- its child manifest (the app's own fs vocabulary).
                local host = host_of(c)
                if list(host.read) then print("    host.read:  " .. list(host.read)) end
                if list(host.write) then print("    host.write: " .. list(host.write)) end
                local cm = type(c.manifest) == "table" and c.manifest or {}
                local cfs = type(cm.fs) == "table" and cm.fs or {}
                if list(cfs.read) then
                    print("    manifest.fs.read:  " .. list(cfs.read) .. "  (under the app dir / @state)")
                end
                if list(cfs.write) then
                    print("    manifest.fs.write: " .. list(cfs.write) .. "  (under the app dir / @state)")
                end
                local smax = num(c.stdin_max or c.stdinMax, 0)
                if smax ~= 0 then
                    print(string.format("    stdin: up to %d bytes from the app", smax))
                end
                if unix_of(c) then
                    print("    local IPC: may connect to any pathname unix socket the service account")
                    print("               can reach (not limited to the one it needs)")
                end
            end
            -- Effective authority (docs/design/exec_broker.md §3.1, §6.4): what
            -- each command can actually reach, per platform, not only what it
            -- declares. Each command has its own sandbox on both (macOS: a
            -- broker per command, docs/design/exec_broker_macos_isolation.md).
            print("  Effective authority (each command its own sandbox):")
            print("    Linux: each command exactly as listed above, plus reads of /usr /lib")
            print("      /lib64 /etc /proc /sys /dev.")
            print("    macOS: each command exactly as listed above, plus the metadata (stat,")
            print("      readlink) of every path; reads of / /usr /bin /sbin /System")
            print("      /private/etc, dyld's cache, /Library/Preferences/Logging,")
            print("      /dev/urandom /dev/random /dev/zero; sysctl reads; user lookups")
            print("    Both: host.write also reads and issues ioctls on its paths;")
            print("      manifest.fs.write writes, truncates and creates, but neither reads")
            print("      nor deletes; an exact-file grant covers that file's inode (a hard")
            print("      link to it anywhere is the same file)")
            print("")
        end
    end

    -- ── Runtime caches (disclosure) ─────────────────────────────────
    --
    -- These paths are NOT declared in the app manifest - they're
    -- infrastructure the runtime decides to memoize for speed (Lua
    -- bytecode, compute AOT, template render functions, signed tool
    -- downloads). The auditor sees them here so "what does this
    -- binary touch on disk" is fully accounted for, matching the
    -- manifest-disclosure property of fs.write / hosts / env.
    --
    -- Registry-driven (tool.cache_kinds() reads hl_cache_registry()
    -- in C) - adding a new cache kind in REGISTRY[] automatically
    -- picks up inspect disclosure here.
    if tool.cache_kinds then
        local kinds = tool.cache_kinds()
        if kinds and #kinds > 0 then
            print("Runtime caches (auto-allowed, not in manifest):")
            for _, k in ipairs(kinds) do
                local kind_tag = k.is_runtime and "runtime" or "system"
                local path = (k.path ~= "") and k.path
                                  or "(unresolvable: HOME unset)"
                print(string.format("  %-12s [%s]  %s",
                                    k.name, kind_tag, path))
                if k.description and k.description ~= "" then
                    print(string.format("                %s",
                                        k.description))
                end
            end
            -- Tool mode strips `os` from the sandbox; use the
            -- dedicated `tool.cache_override()` binding instead.
            local override = tool.cache_override and tool.cache_override() or nil
            if override then
                print("  HULL_CACHE_DIR override active: " .. override)
                print("  (runtime caches re-rooted for per-app isolation;")
                print("   `tools` is unaffected - durable signed downloads)")
            end
            print("  manage via `hull cache list|prune|clear`,")
            print("  opt out with HULL_NO_CACHE=1")
            print("")
        end
    end

    -- ── Migrations ──────────────────────────────────────────────────
    if sig.files then
        local migration_names = {}
        for name in pairs(sig.files) do
            if name:match("^migrations/") then
                migration_names[#migration_names + 1] = name
            end
        end
        if #migration_names > 0 then
            table.sort(migration_names)
            print("Migrations (" .. #migration_names .. "):")
            for _, name in ipairs(migration_names) do
                print("  " .. name .. "  " .. sig.files[name])
            end
            print("")
        end
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

-- The tool dispatcher (src/hull/tool.c) invokes the returned main() only when
-- this module is the entry command it was asked to run. A module that is
-- require()'d as a dependency (e.g. by an app during manifest extraction in
-- the tool VM) hands its main() back but is never called, so it can't run
-- against the wrong argv.
return main
