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
    return tool.hull_cache_disabled("AOT") or tool.hull_cache_disabled(nil)
end

-- wamrc version capture — memoized per (binary path, process).
local wamrc_version_memo = {}

function M.wamrc_version(wamrc_bin)
    if not wamrc_bin then return nil end
    if wamrc_version_memo[wamrc_bin] then
        return wamrc_version_memo[wamrc_bin]
    end
    -- wamrc has no --version flag; `--help` first line carries the
    -- identifying banner ("WAMR Ahead-of-Time compiler"). Combine
    -- with binary mtime for change detection — banner is stable
    -- across builds but mtime moves whenever wamrc is rebuilt.
    local mtime = tool.file_mtime(wamrc_bin) or 0
    local banner = tool.spawn_read({wamrc_bin, "--help"}) or ""
    local first = banner:match("([^\n]+)") or ""
    local id = string.format("%s|mtime=%d", first:sub(1, 80), mtime)
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
