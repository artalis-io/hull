--
-- hull.verify — Verify app signature (dual-layer + v0.1.3 gethull)
--
-- Usage: hull verify [options] [app_dir]
--   --platform-key <file|url>   Per-app platform key (the v0.1.2 platforms
--                               object is signed by the app developer here,
--                               not by gethull.dev — distinct from the
--                               gethull layer below)
--   --developer-key <file|url>  Developer public key for the app layer
--   --gethull-key <file|url>    Cross-check: the file's pubkey must match
--                               this hull's embedded HL_PLATFORM_PUBKEY_HEX
--                               AND the signature must verify against it.
--                               Defends against tampered hull binaries
--                               where the embedded key has been swapped.
--                               Note: stricter than verify.js's same-named
--                               flag, which uses the file as an override
--                               (JS can't reach the embedded key).
--   --no-verify-platform        Skip the v0.1.3 gethull platform-sig check
--                               (package.sig.platform.gethull). Use this for
--                               apps built with a dev hull (no embedded
--                               manifest) or with a fork that signs platform
--                               with its own key.
--
-- Verification layers, in order:
--   1. gethull layer (v0.1.3): package.sig.platform.gethull — the signed
--      libhull_platform.a manifest carried forward from the hull binary
--      that ran `hull build`. Pubkey is the build-time-pinned
--      HL_PLATFORM_PUBKEY_HEX (queried via tool.platform_pubkey()).
--   2. app's per-build platform layer (v0.1.2): package.sig.platform.{
--      platforms, public_key, signature} — the JSON object the developer
--      signed alongside their app.
--   3. app layer: package.sig.{files, signature, public_key} — Ed25519
--      over the canonical-JSON payload of {binary_hash, build, files,
--      manifest, platform, trampoline_hash[, modules_resolved]}.
--   4. file hashes — each path in files{} re-hashed and compared.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")

-- Placeholder until the real gethull.dev platform key is pinned in this file.
-- Until then, `hull verify` requires the caller to pass --platform-key PATH
-- (a file containing the hex-encoded public key) and will refuse to fall
-- back to the all-zeros sentinel.
local GETHULL_DEV_PLATFORM_KEY_PLACEHOLDER =
    "0000000000000000000000000000000000000000000000000000000000000000"

local function read_file(path)
    return tool.read_file(path)
end

-- Resolve a key SOURCE to its hex pubkey. Returns nil ONLY when no source was
-- given (the caller then legitimately skips that key check). A source that IS
-- given but cannot be read is a HARD ERROR (fail-closed): silently returning nil
-- there would skip the operator-intended key pin (a fail-open security bypass).
local function read_key(source)
    if not source then return nil end

    -- URL fetch is not supported: the tool sandbox's spawn allowlist has no
    -- network client (curl et al. are not permitted), so a `https://` source
    -- could only ever fail. Error clearly instead of failing open.
    if source:sub(1, 8) == "https://" or source:sub(1, 7) == "http://" then
        tool.stderr("hull verify: URL key sources are not supported "
                    .. "(no network client in the tool sandbox).\n"
                    .. "Download the key and pass a local file path instead: "
                    .. source .. "\n")
        tool.exit(1)
    end

    -- File path
    local data = read_file(source)
    if not data then
        tool.stderr("hull verify: cannot read key file: " .. source .. "\n")
        tool.exit(1)
    end
    local hex = data:match("^(%x+)")
    if not hex then
        tool.stderr("hull verify: key file has no valid hex public key: "
                    .. source .. "\n")
        tool.exit(1)
    end
    return hex
end

local function parse_args()
    local opts = {
        app_dir = ".",
        platform_key = nil,
        developer_key = nil,
        gethull_key = nil,
        no_verify_platform = false,
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
        elseif a == "--gethull-key" then
            i = i + 1
            opts.gethull_key = arg[i]
        elseif a == "--no-verify-platform" then
            opts.no_verify_platform = true
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

    -- ── gethull layer (v0.1.3) ─────────────────────────────────────
    -- The signed libhull_platform.a manifest carried forward from the
    -- hull binary that ran `hull build`. The signing key is gethull.dev's
    -- platform key, pinned at build time in HL_PLATFORM_PUBKEY_HEX.
    -- This whole block is no-op when:
    --   * --no-verify-platform is set, or
    --   * this verify hull was built with the all-zeros placeholder
    --     pubkey (dev hulls / forks without their own pinned key) —
    --     we cannot validate anyway, and apps built by such a hull
    --     legitimately have no gethull block.
    --
    -- --gethull-key <file>: cross-check semantics (stricter than the
    -- JS verifier, which can't reach the embedded pubkey and so uses
    -- the file as an override). When passed:
    --   1. The signature MUST verify against the embedded
    --      HL_PLATFORM_PUBKEY_HEX (the normal gethull check).
    --   2. AND the file's key MUST match the embedded pubkey.
    -- Both gates have to pass. Catches the case where someone
    -- tampered with this hull's embedded pubkey AND produced a
    -- matching signature: the operator's expected key (from the
    -- file) won't match the tampered embedded one.
    local verify_pubkey_hex = tool.platform_pubkey()
    if opts.no_verify_platform then
        print("gethull layer: SKIPPED (--no-verify-platform)")
    elseif not verify_pubkey_hex then
        print("gethull layer: SKIPPED (this hull has no pinned platform " ..
            "pubkey - placeholder build)")
    elseif sig.platform and sig.platform.gethull and
           sig.platform.gethull.manifest and sig.platform.gethull.signature then
        -- Optional cross-check: --gethull-key
        if opts.gethull_key then
            local expected_hex = read_key(opts.gethull_key)
            if not expected_hex then
                tool.stderr("gethull layer: FAILED - could not read " ..
                    "--gethull-key file " .. opts.gethull_key .. "\n")
                issues = issues + 1
            elseif expected_hex ~= verify_pubkey_hex then
                tool.stderr("gethull layer: FAILED - --gethull-key does " ..
                    "not match this hull's embedded HL_PLATFORM_PUBKEY_HEX\n")
                tool.stderr("  expected: " .. expected_hex:sub(1, 16) .. "...\n")
                tool.stderr("  embedded: " .. verify_pubkey_hex:sub(1, 16) .. "...\n")
                tool.stderr("  this hull binary may have been tampered with,\n")
                tool.stderr("  or you passed the wrong --gethull-key file\n")
                issues = issues + 1
            end
        end
        local gethull_ok = crypto.ed25519_verify(
            sig.platform.gethull.manifest,
            sig.platform.gethull.signature,
            verify_pubkey_hex)
        if gethull_ok then
            print("gethull layer: VALID (signed by gethull.dev)")
        else
            tool.stderr("gethull layer: FAILED - signature invalid\n")
            tool.stderr("  the embedded libhull_platform.a does not " ..
                "match what gethull.dev signed at release time\n")
            issues = issues + 1
        end
    elseif not is_legacy then
        tool.stderr("gethull layer: MISSING - package.sig has no " ..
            "platform.gethull block\n")
        tool.stderr("  hint: rebuild with a hull v0.1.3+ that has " ..
            "platform-sig wired through, or pass --no-verify-platform\n")
        issues = issues + 1
    end

    -- ── v0.1.2 per-app platform layer (self-consistency only) ──────
    -- The developer's `hull sign-platform` step produced this block
    -- with their own platform key. We verify the signature is
    -- self-consistent (covers the platforms object with the embedded
    -- pubkey) but do NOT pin against any upstream key — that pinning
    -- moved to the v0.1.3 gethull layer above, which uses a signed
    -- manifest from the gethull release pipeline. --platform-key is
    -- still honored as an explicit override for forks that want to
    -- compare against an expected developer pubkey.
    if sig.platform and sig.platform.signature and sig.platform.public_key then
        local platform_key_hex = read_key(opts.platform_key)
        if platform_key_hex and platform_key_hex ~=
                GETHULL_DEV_PLATFORM_KEY_PLACEHOLDER and
                sig.platform.public_key ~= platform_key_hex then
            tool.stderr("Platform layer: WARNING - key does not match " ..
                "--platform-key (expected " .. platform_key_hex:sub(1, 16) ..
                "..., got " .. sig.platform.public_key:sub(1, 16) .. "...)\n")
            issues = issues + 1
        end

        local plat_payload = json.encode(sig.platform.platforms)
        local plat_ok = crypto.ed25519_verify(plat_payload,
            sig.platform.signature, sig.platform.public_key)
        if plat_ok then
            print("Platform layer: VALID (self-consistent)")
        else
            tool.stderr("Platform layer: FAILED - signature invalid\n")
            issues = issues + 1
        end

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
            tool.stderr("App layer: WARNING - developer key mismatch\n")
            issues = issues + 1
        end
    end

    -- Verify app signature
    local payload
    if sig.binary_hash then
        -- New package.sig format. Include modules_resolved if present
        -- (added in the package & module system roadmap), omit otherwise
        -- so signatures written by older builds still verify.
        local reconstructed = {
            binary_hash = sig.binary_hash,
            build = sig.build,
            files = sig.files,
            manifest = sig.manifest,
            platform = sig.platform,
            trampoline_hash = sig.trampoline_hash,
        }
        if sig.modules_resolved then
            reconstructed.modules_resolved = sig.modules_resolved
        end
        payload = json.encode(reconstructed)
    else
        -- Legacy hull.sig format
        payload = json.encode({
            files = sig.files,
            manifest = sig.manifest,
        })
    end

    local ok = crypto.ed25519_verify(payload, sig.signature, sig.public_key)
    if not ok then
        tool.stderr("App layer: FAILED - signature is invalid\n")
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
        -- Path traversal defense: reject suspicious file names
        if name:find("%.%.") or name:sub(1, 1) == "/" then
            tool.stderr("  Suspicious file path: " .. name .. "\n")
            issues = issues + 1
            goto continue_files
        end
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
        ::continue_files::
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
        tool.stderr("hull verify: FAILED - " .. issues .. " issue(s) found\n")
        tool.exit(1)
    end

    print("App layer: VALID")
    print("")
    print("hull verify: OK - all checks passed")
end

-- The tool dispatcher (src/hull/tool.c) invokes the returned main() only when
-- this module is the entry command it was asked to run. A module that is
-- require()'d as a dependency (e.g. by an app during manifest extraction in
-- the tool VM) hands its main() back but is never called, so it can't run
-- against the wrong argv.
return main
