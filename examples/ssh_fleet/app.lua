-- ssh_fleet - run one command across a list of hosts, over hull/ssh.
--
-- A small fleet tool, which is what an SSH client in a single-binary runtime
-- is mostly for: no ssh-agent, no dot-ssh directory, no shelling out to a
-- system ssh, and a manifest that says in advance which hosts and which
-- logins this program may reach.
--
--   hull run app.lua -- web1.internal web2.internal
--   HULL_SSH_COMMAND="systemctl is-active app" hull run app.lua -- web1.internal
--   hull run app.lua -- --accept-new web1.internal
--   HULL_SSH_TUNNEL=relay.example.com:443 hull run app.lua -- web1.internal
--
-- Four things worth reading this for:
--
--   1. The MANIFEST is the allowlist. ssh.connect.hosts/ports/users is
--      checked in C before a socket exists. A host not named there is
--      refused no matter what argv says, so a bug in this file cannot turn
--      into a connection somewhere else.
--   2. Host keys are NOT trusted automatically. An unknown host comes back
--      as host_unknown carrying its fingerprint; this program prints it and
--      stops unless the operator passed --accept-new. That is the whole
--      trust-on-first-use decision, made explicitly, in one place.
--   3. The trust store is the APPLICATION's. Hull ships no on-disk
--      known_hosts, because where trust lives is a deployment decision.
--      Here it is a JSON file beside the app.
--   4. A non-zero remote exit is a STATUS, not an error: "systemctl
--      is-active" answering 3 is the answer, not a failure to ask.

local ssh  = require("hull.ssh")
local fs   = require("hull.fs")
local json = require("hull.json")

app.manifest({
    -- No "hull/env@1": ctx.env is handed to app.main by the runtime, and the
    -- manifest's own `env` list below is what gates which variables it can
    -- see. Declaring the MODULE as well would widen the capability surface
    -- for an import this app never makes - `hull check` says so.
    modules = { "hull/ssh@1", "hull/fs@1", "hull/json@1" },
    env = { "HULL_SSH_KEY", "HULL_SSH_USER", "HULL_SSH_COMMAND",
            "HULL_SSH_TUNNEL", "HULL_SSH_KEY_PASSPHRASE" },
    fs = {
        read  = { "id_ed25519", "known_hosts.json" },
        write = { "known_hosts.json" },
    },
    ssh = {
        -- Widen these to YOUR fleet. A suffix glob is accepted
        -- ("*.internal"), an exact name is better, and "*" is a decision
        -- rather than a default.
        connect = {
            hosts = { "*.internal", "127.0.0.1" },
            ports = { 22 },
            users = { "deploy", "root" },
        },
        -- Only needed when the fleet is reached through a WebSocket relay
        -- (a Cloudflare Access tunnel, say). Dropping this section makes
        -- HULL_SSH_TUNNEL unusable, which is the point of declaring it:
        -- the relay is a second grant, not a widening of the first.
        tunnel = { hosts = { "*.example.com" }, ports = { 443 } },
    },
})

local TRUST_FILE = "known_hosts.json"

-- The trust store, persisted as JSON. The stored blobs are RAW host-key
-- bytes, so they are hex-encoded on the way to disk rather than written as
-- if they were text.
local function to_hex(s)
    return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

local function from_hex(s)
    -- Checked, not just decoded. gsub leaves anything that does not match
    -- exactly in place, so a truncated or edited entry would decode to a
    -- blob that is WRONG rather than refused - and the next connection would
    -- report HOST KEY CHANGED, which is the loudest possible way to say
    -- "your file is damaged".
    if type(s) ~= "string" or #s == 0 or #s % 2 ~= 0 or s:find("%X") then
        return nil
    end
    return (s:gsub("%x%x", function(h) return string.char(tonumber(h, 16)) end))
end

local function load_trust()
    local text = fs.read(TRUST_FILE)
    if not text or text == "" then return ssh.memory_store() end
    local ok, decoded = pcall(json.decode, text)
    if not ok or type(decoded) ~= "table" then
        -- A damaged store is not an empty store. Silently starting over
        -- would turn every host into a first contact and accept whatever
        -- answered; stop instead, and let a person look at it.
        error(TRUST_FILE .. " is not readable JSON; refusing to continue")
    end
    local seed = {}
    for host, hex in pairs(decoded) do
        local blob = from_hex(hex)
        if not blob then
            error(TRUST_FILE .. ": the entry for " .. tostring(host)
                  .. " is not valid hex; refusing to continue")
        end
        seed[host] = blob
    end
    return ssh.memory_store(seed)
end

local function save_trust(trust)
    local out = {}
    for host, blob in pairs(trust.entries()) do out[host] = to_hex(blob) end
    fs.write(TRUST_FILE, json.encode(out))
end

-- The relay, when one is configured: "host:port", TLS always on. A plaintext
-- relay would carry the SSH stream in the clear across whatever sits between
-- here and it, which is exactly the hop a relay exists to cross.
local function parse_tunnel(spec)
    if not spec or spec == "" then return nil end
    local host, port = spec:match("^([^:]+):(%d+)$")
    if not host then
        error("HULL_SSH_TUNNEL must look like relay.example.com:443")
    end
    return {
        host = host,
        port = tonumber(port),
        tls = true,
        -- A Cloudflare Access service token goes here:
        --   "Cf-Access-Client-Id: ...", "Cf-Access-Client-Secret: ..."
        -- Left empty, the relay decides for itself whether to upgrade.
        headers = {},
    }
end

local function run_one(ctx, host, opts)
    local conn, err = ssh.connect{
        host   = host,
        port   = 22,
        user   = opts.user,
        key    = opts.key,
        trust  = opts.trust,
        tunnel = opts.tunnel,
    }

    if not conn and err.code == "host_unknown" then
        ctx.stderr:write(host .. ": unknown host key " .. err.fingerprint .. "\n")
        if not opts.accept_new then
            ctx.stderr:write("  re-run with --accept-new to trust it\n")
            return 1
        end
        ssh.accept_host(opts.trust, host, err.key_blob)
        save_trust(opts.trust)
        -- A second connect, not a resumed one: the first was refused before
        -- authentication, so there is nothing to resume.
        conn, err = ssh.connect{
            host = host, port = 22, user = opts.user,
            key = opts.key, trust = opts.trust, tunnel = opts.tunnel,
        }
    end

    if not conn then
        -- host_changed is the one that means STOP. The others are a
        -- connection that did not happen; this one is a host presenting a
        -- key that is not the key it had.
        if err.code == "host_changed" then
            ctx.stderr:write(host .. ": HOST KEY CHANGED - was "
                .. tostring(err.stored_fingerprint) .. ", now "
                .. tostring(err.fingerprint) .. "\n")
            return 2
        end
        ctx.stderr:write(host .. ": " .. tostring(err.code)
            .. (err.detail and (" (" .. tostring(err.detail) .. ")") or "")
            .. "\n")
        return 1
    end

    local r, rerr = conn:exec(opts.command)
    if not r then
        conn:close()
        ctx.stderr:write(host .. ": " .. tostring(rerr.code) .. "\n")
        return 1
    end

    -- Output is prefixed per host, which is what keeps the fleet view
    -- readable once several of them answer.
    for line in (r.stdout or ""):gmatch("[^\n]+") do
        ctx.stdout:write(host .. ": " .. line .. "\n")
    end
    for line in (r.stderr or ""):gmatch("[^\n]+") do
        ctx.stderr:write(host .. ": " .. line .. "\n")
    end

    local stats = conn:stats()
    ctx.stderr:write(string.format(
        "%s: exit %d, %s, %d bytes, %d rekey(s)\n",
        host, r.status, conn:negotiated().cipher_c2s,
        stats.bytes_sent + stats.bytes_received, stats.rekeys))

    conn:close()
    return r.status
end

local function usage(stderr)
    stderr:write("usage: hull run app.lua -- [--accept-new] HOST...\n")
    stderr:write("  HULL_SSH_KEY      private key path (default id_ed25519)\n")
    stderr:write("  HULL_SSH_USER     login (default deploy)\n")
    stderr:write("  HULL_SSH_COMMAND  command (default uname -a)\n")
    stderr:write("  HULL_SSH_TUNNEL   relay host:port for a WebSocket tunnel\n")
end

app.main(function(ctx)
    local hosts, accept_new = {}, false
    for _, a in ipairs(ctx.args) do
        if a == "--accept-new" then
            accept_new = true
        elseif a == "-h" or a == "--help" then
            usage(ctx.stderr)
            return 0
        elseif a:sub(1, 1) == "-" then
            ctx.stderr:write("unknown flag: " .. a .. "\n")
            return 2
        else
            hosts[#hosts + 1] = a
        end
    end
    if #hosts == 0 then
        usage(ctx.stderr)
        return 2
    end

    local key_file = ctx.env.HULL_SSH_KEY or "id_ed25519"
    local key_text = fs.read(key_file)
    if not key_text then
        ctx.stderr:write("cannot read " .. key_file
            .. " (add it to manifest.fs.read)\n")
        return 2
    end

    -- The passphrase is NAMED, not carried: passphrase_env hands the
    -- variable NAME to C, which reads it, derives the key and scrubs the
    -- buffer. A passphrase read into a Lua string could not be wiped.
    local ok, key = pcall(ssh.load_key, key_text, {
        passphrase_env = ctx.env.HULL_SSH_KEY_PASSPHRASE
                         and "HULL_SSH_KEY_PASSPHRASE" or nil,
    })
    if not ok then
        ctx.stderr:write("cannot load " .. key_file .. ": " .. tostring(key) .. "\n")
        return 2
    end

    local opts = {
        key        = key,
        user       = ctx.env.HULL_SSH_USER or "deploy",
        command    = ctx.env.HULL_SSH_COMMAND or "uname -a",
        tunnel     = parse_tunnel(ctx.env.HULL_SSH_TUNNEL),
        trust      = load_trust(),
        accept_new = accept_new,
    }

    -- The worst status wins, so a wrapper script can branch on one number.
    local worst = 0
    for _, host in ipairs(hosts) do
        local status = run_one(ctx, host, opts)
        if status > worst then worst = status end
    end
    return worst
end)
