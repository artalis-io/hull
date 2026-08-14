--
-- hull.aot_cache — Content-addressed AOT artifact cache.
--
-- Memoizes `wamrc` output across `hull build` invocations so the same
-- (wasm, arch, memory64, wamrc-version) tuple compiles at most once
-- per machine.
--
-- Backed by hull/blob_store in keyed mode (see
-- hl_blob_store_put_keyed in include/hull/blob_store.h). The store
-- lives at $HOME/.hull/blobs/runtime/compute-aot/ — shared
-- system-wide across every Hull app on the host, partitioned away
-- from app blobs by the `runtime/` subtree.
--
-- The cache is runtime-infrastructure, not part of any app's
-- manifest. The tool-mode sandbox auto-allows the path; apps
-- neither see nor depend on it.
--
-- Public surface (CLI-only; not exposed to app code):
--
--   M.key(wasm_path, arch, mem64, wamrc_version)
--     → string|nil       64-char hex digest the store uses for this
--                        artifact, or nil on read failure.
--
--   M.lookup(key, dest_path)
--     → bool             Copy cached blob to `dest_path`. False on
--                        miss / opt-out / copy failure.
--
--   M.store(key, src_path)
--     → bool             Read `src_path` into the cache under `key`.
--                        Best-effort — returns false silently on
--                        any failure.
--
--   M.wamrc_version(wamrc_bin)
--     → string|nil       Captures the wamrc identity string for the
--                        cache key. Memoized per process.
--
-- Honors HULL_NO_CACHE=1 and HULL_NO_AOT_CACHE=1 — `lookup` and
-- `store` are no-ops when disabled. Callers can additionally pass
-- `--no-cache` on `hull build` (handled in build.lua, not here).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

local CACHE_KIND = "compute-aot"

local function disabled()
    -- hull_cache_disabled("AOT") already checks HULL_NO_CACHE first
    -- (cache_dir.c:198 short-circuits before the per-kind check),
    -- so the global kill-switch is covered by this single call.
    return tool.hull_cache_disabled("AOT")
end

-- wamrc identity capture — memoized per (binary path, process).
--
-- Cache invalidation correctness depends on this being a true
-- content fingerprint. Two wamrc binaries that produce different
-- AOT bytes must produce different ids here, otherwise the cache
-- silently serves the wrong artifact for a swapped compiler.
--
-- Earlier versions used `wamrc --help`'s first line + binary mtime.
-- That fingerprint failed under realistic operational scenarios —
-- `cp -p`, `rsync --times`, and tar restore-from-backup all
-- preserve mtime, and wamrc's --help banner has historically been
-- stable across releases. A swapped binary with preserved mtime
-- collided to the same id as its predecessor.
--
-- sha256(contents) is the only fingerprint that's correct by
-- construction. wamrc binaries are tens of MB, but this runs once
-- per process and is memoized; the build pipeline is doing
-- megabyte-scale work elsewhere anyway.
local wamrc_version_memo = {}

function M.wamrc_version(wamrc_bin)
    if not wamrc_bin then return nil end
    if wamrc_version_memo[wamrc_bin] then
        return wamrc_version_memo[wamrc_bin]
    end
    local bytes = tool.read_file(wamrc_bin)
    if not bytes then
        -- Fail closed: no fingerprint → no cache key → no cache.
        -- Better than fingerprinting `nil` and colliding every
        -- failing-probe run together.
        return nil
    end
    local id = "wamrc-sha256=" .. crypto.sha256(bytes)
    wamrc_version_memo[wamrc_bin] = id
    return id
end

-- Compose the cache key from inputs that can change the AOT output.
--
-- Components:
--   wasm_sha    — content hash of the .wasm input
--   arch        — wamrc target arch tag (x86_64, aarch64)
--   mem64_flag  — 1 if --enable-memory64 will be passed
--   wamrc_id    — version banner + binary mtime
--
-- Folded into a single SHA-256, hex-encoded — the blob_store keyed
-- mode expects a 64-char lowercase hex id.
function M.key(wasm_path, arch, mem64, wamrc_version)
    if not wasm_path or not arch then return nil end
    local wasm_bytes = tool.read_file(wasm_path)
    if not wasm_bytes then return nil end
    local wasm_sha = crypto.sha256(wasm_bytes)
    local payload = table.concat({
        wasm_sha,
        "|arch=", arch,
        "|mem64=", mem64 and "1" or "0",
        "|shared_heap=1",   -- compute AOT is always built with --enable-shared-heap
                            -- (spans + compute.segment); invalidates pre-fix entries
        "|wamrc=", wamrc_version or "unknown",
    })
    return crypto.sha256(payload)
end

function M.lookup(key, dest_path)
    if not key or not dest_path or disabled() then return false end
    if not tool.blob_store_exists(CACHE_KIND, key) then return false end
    return tool.blob_store_get_to(CACHE_KIND, key, dest_path)
end

function M.store(key, src_path)
    if not key or not src_path or disabled() then return false end
    return tool.blob_store_put_from(CACHE_KIND, key, src_path)
end

-- Cache-disabled probe for callers that want to log the reason.
function M.is_enabled() return not disabled() end

return M
