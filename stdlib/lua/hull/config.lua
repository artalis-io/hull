--- Typed application configuration over the environment.
--
-- A thin, fail-fast layer over the `env` capability: read a setting with a
-- type, a default, and optional required-ness, instead of hand-rolling
-- `tonumber(env.get("PORT")) or 8080` at every call site. The env allowlist
-- still applies - a name must be in `manifest.env` for `env.get` to return it
-- (an un-allowlisted name reads as absent, so it falls back to the default or,
-- if `required`, throws).
--
-- Errors follow the stdlib convention (docs/stdlib_style.md section 1): a
-- missing required value or an uncoercible value THROWS; a present-and-valid or
-- absent-with-default value is returned.
--
-- @module hull.config
-- @license AGPL-3.0-or-later
-- @usage
--   local config = require("hull.config")
--   local port   = config.get("PORT", { type = "integer", default = 8080 })
--   local dsn    = config.require("DATABASE_URL")          -- throws if unset
--   local debug  = config.get("DEBUG", { type = "boolean", default = false })

local env = require("hull.env")
local fs = require("hull.fs")

local config = {}

-- Values loaded from a .env file by config.load_dotenv. ONLY names that are in
-- the manifest env allowlist are ever stored here (see load_dotenv), so falling
-- back to this map can never widen what config exposes beyond manifest.env.
local _dotenv = {}

-- "1"/"true"/"yes"/"on" -> true; "0"/"false"/"no"/"off"/"" -> false (case-
-- insensitive). Anything else throws (an ambiguous boolean is a config error).
local TRUTHY = { ["1"] = true, ["true"] = true, ["yes"] = true, ["on"] = true }
local FALSY = { ["0"] = true, ["false"] = true, ["no"] = true, ["off"] = true, [""] = true }

local function coerce(name, raw, ty)
    if ty == nil or ty == "string" then
        return raw
    elseif ty == "boolean" then
        local k = raw:lower()
        if TRUTHY[k] then return true end
        if FALSY[k] then return false end
        error("config: " .. name .. " is not a boolean: '" .. raw .. "'")
    elseif ty == "number" then
        local n = tonumber(raw)
        if n == nil then error("config: " .. name .. " is not a number: '" .. raw .. "'") end
        return n
    elseif ty == "integer" then
        local n = tonumber(raw)
        if n == nil or n ~= math.floor(n) then
            error("config: " .. name .. " is not an integer: '" .. raw .. "'")
        end
        return math.floor(n)
    end
    error("config: unknown type '" .. tostring(ty) .. "' for " .. name)
end

--- Read a configuration value.
--
-- @tparam string name  Environment variable name (must be in `manifest.env`).
-- @tparam[opt] table opts
--   - `type`     `"string"` (default) / `"number"` / `"integer"` / `"boolean"`.
--   - `default`  Returned when the value is absent. NOT type-coerced (pass a
--                real `8080`, not `"8080"`).
--   - `required` (boolean) Throw if the value is absent (default `false`).
-- @return The typed value, or `default` when absent.
function config.get(name, opts)
    opts = opts or {}
    -- Precedence: the real process env (via the allowlisted env cap) wins; a
    -- .env value (also allowlist-gated at load time) is a fallback for a
    -- declared-but-unset name; then the default.
    local raw = env.get(name)
    if (raw == nil or raw == "") and _dotenv[name] ~= nil then
        raw = _dotenv[name]
    end
    if raw == nil or raw == "" then
        if opts.required then
            error("config: required setting " .. tostring(name) .. " is not set")
        end
        return opts.default
    end
    return coerce(name, raw, opts.type)
end

local function strip_quotes(v)
    if #v >= 2 then
        local q = v:sub(1, 1)
        if (q == '"' or q == "'") and v:sub(-1) == q then
            return v:sub(2, -2)
        end
    end
    return v
end

--- Load a `.env` file so its values back declared settings.
--
-- TWO gates, both enforced:
--   1. The file is read through the `fs` capability, so the app must declare
--      the path in `manifest.fs.read` (a denied / missing file is a silent
--      no-op returning 0 - `.env` is optional local-dev convenience).
--   2. A `KEY=value` line is applied ONLY when `KEY` is in the manifest env
--      allowlist (`env.allowed(KEY)`). Keys not in `manifest.env` are ignored,
--      so `.env` can never introduce a value for a name the manifest doesn't
--      already permit.
--
-- The real process environment always overrides a `.env` value (`.env` is
-- defaults for local dev). Call once at startup, before the first
-- `config.get`. Parses `KEY=value`, `#` comments, blank lines, an optional
-- `export ` prefix, and surrounding single/double quotes.
--
-- @tparam[opt=".env"] string path  File path (must be in `manifest.fs.read`).
-- @treturn integer  Number of allowlisted keys applied.
function config.load_dotenv(path)
    path = path or ".env"
    local content = fs.read(path)   -- (nil, err) when denied / missing
    if not content then return 0 end
    local n = 0
    for line in (content .. "\n"):gmatch("([^\n]*)\n") do
        local s = line:gsub("^%s+", ""):gsub("%s+$", "")
        if s ~= "" and s:sub(1, 1) ~= "#" then
            s = s:gsub("^export%s+", "")
            local eq = s:find("=", 1, true)
            if eq then
                local k = (s:sub(1, eq - 1)):gsub("%s+$", "")
                local v = strip_quotes((s:sub(eq + 1)):gsub("^%s+", ""))
                if k ~= "" and env.allowed(k) then
                    _dotenv[k] = v
                    n = n + 1
                end
            end
        end
    end
    return n
end

--- Read a required configuration value (throws if absent). Shorthand for
-- `config.get(name, { required = true, type = opts.type })`.
-- @tparam string name
-- @tparam[opt] table opts  Accepts `type` (see @{config.get}).
-- @return The typed value.
function config.require(name, opts)
    opts = opts or {}
    return config.get(name, { type = opts.type, required = true })
end

return config
