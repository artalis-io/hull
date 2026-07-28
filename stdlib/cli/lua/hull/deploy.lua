--
-- hull.deploy — Deployment config generator
--
-- Usage: hull deploy <target> [app_dir] [options]
--
-- Targets:
--   dockerfile  Generate Dockerfile + .dockerignore
--   systemd     Generate systemd service unit + install.sh
--   fly         Generate fly.toml (+ Dockerfile if missing)
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

-- ── Argument parsing ─────────────────────────────────────────────────

local function parse_args()
    local opts = {
        target   = nil,
        app_dir  = ".",
        port     = 3000,
        name     = nil,
        output   = nil,
        force    = false,
        -- dockerfile
        scratch    = true,
        distroless = false,
        sign       = false,
        -- systemd
        user        = "hull",
        install_dir = nil,
        data_dir    = nil,
        -- fly
        region = "iad",
        memory = 256,
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--port" or a == "-p" then
            i = i + 1; opts.port = tonumber(arg[i]) or 3000
        elseif a == "--name" then
            i = i + 1; opts.name = arg[i]
        elseif a == "-o" or a == "--output" then
            i = i + 1; opts.output = arg[i]
        elseif a == "--force" then
            opts.force = true
        elseif a == "--scratch" then
            opts.scratch = true; opts.distroless = false
        elseif a == "--distroless" then
            opts.distroless = true; opts.scratch = false
        elseif a == "--sign" then
            opts.sign = true
        elseif a == "--user" then
            i = i + 1; opts.user = arg[i]
        elseif a == "--install-dir" then
            i = i + 1; opts.install_dir = arg[i]
        elseif a == "--data-dir" then
            i = i + 1; opts.data_dir = arg[i]
        elseif a == "--region" then
            i = i + 1; opts.region = arg[i]
        elseif a == "--memory" then
            i = i + 1; opts.memory = tonumber(arg[i]) or 256
        elseif a == "-h" or a == "--help" then
            print("Usage: hull deploy <target> [app_dir] [options]")
            print("")
            print("Targets:")
            print("  dockerfile   Generate Dockerfile + .dockerignore")
            print("  systemd      Generate systemd service unit + install.sh")
            print("  fly          Generate fly.toml (+ Dockerfile if missing)")
            print("")
            print("Options:")
            print("  --port N          Listen port (default: 3000)")
            print("  --name NAME       Service/image name (default: dir basename)")
            print("  -o DIR            Output directory (default: app_dir)")
            print("  --force           Overwrite existing files")
            print("")
            print("Dockerfile options:")
            print("  --scratch         FROM scratch (default)")
            print("  --distroless      FROM gcr.io/distroless/static-debian12")
            print("  --sign            Add hull verify step in build stage")
            print("")
            print("systemd options:")
            print("  --user NAME       System user (default: hull)")
            print("  --install-dir P   Binary location (default: /opt/hull/<name>)")
            print("  --data-dir P      Data directory (default: /var/lib/hull/<name>)")
            print("")
            print("fly options:")
            print("  --region CODE     Primary region (default: iad)")
            print("  --memory N        MB (default: 256)")
            tool.exit(0)
        elseif a:sub(1, 1) ~= "-" then
            if not opts.target then
                opts.target = a
            else
                opts.app_dir = a
            end
        end
        i = i + 1
    end

    if not opts.target then
        tool.stderr("hull deploy: target required (dockerfile, systemd, fly)\n")
        tool.stderr("Run 'hull deploy --help' for usage.\n")
        tool.exit(1)
    end

    if opts.target ~= "dockerfile" and opts.target ~= "systemd" and opts.target ~= "fly" then
        tool.stderr("hull deploy: unknown target '" .. opts.target .. "'\n")
        tool.stderr("Valid targets: dockerfile, systemd, fly\n")
        tool.exit(1)
    end

    -- Derive defaults
    if not opts.name then
        opts.name = opts.app_dir:match("([^/]+)/?$") or "app"
    end
    if not opts.output then
        opts.output = opts.app_dir
    end
    if not opts.install_dir then
        opts.install_dir = "/opt/hull/" .. opts.name
    end
    if not opts.data_dir then
        opts.data_dir = "/var/lib/hull/" .. opts.name
    end

    -- L-6: the generated install.sh interpolates these values into shell
    -- `echo "..."` strings. Reject anything that breaks the quoting so a
    -- malicious `--user 'foo"; rm -rf /'` can't smuggle a command via
    -- sudo when the user later runs install.sh. The actual sandbox is
    -- the user reviewing the generated file before running it, but a
    -- well-formed file is a cheap second line of defence.
    local function validate_ident(value, label)
        if type(value) ~= "string" or value == "" then
            tool.stderr("hull deploy: " .. label .. " cannot be empty\n")
            tool.exit(1)
        end
        if value:find("[^%w./_%-]") then
            tool.stderr("hull deploy: " .. label .. " contains unsafe characters: '" .. value .. "'\n")
            tool.stderr("Allowed: alphanumerics, '.', '/', '_', '-'.\n")
            tool.exit(1)
        end
        -- Phase 6 audit L-1: reject leading '-' so the value can't be
        -- interpreted as a flag by tools the generated install.sh
        -- invokes (useradd, chown, mkdir, systemctl).
        if value:sub(1, 1) == "-" then
            tool.stderr("hull deploy: " .. label .. " cannot start with '-': '" .. value .. "'\n")
            tool.exit(1)
        end
    end
    if opts.user then validate_ident(opts.user, "--user") end
    validate_ident(opts.name, "--name")
    validate_ident(opts.install_dir, "--install-dir")
    validate_ident(opts.data_dir, "--data-dir")

    return opts
end

-- ── App introspection ────────────────────────────────────────────────

local function dir_exists(path)
    -- tool.find_files returns nil for missing dirs, use tool.file_exists on dir
    return tool.file_exists(path)
end

local function count_files(dir, pattern)
    if not dir_exists(dir) then return 0 end
    local files = tool.find_files(dir, pattern)
    if not files then return 0 end
    return #files
end

local function introspect_app(app_dir)
    local info = {
        runtime        = nil,
        entry          = nil,
        manifest       = nil,
        has_migrations = false,
        has_static     = false,
        has_compute    = false,
        has_shaders    = false,
        has_signature  = false,
        has_database   = false,
        env_vars       = {},
        hosts          = {},
    }

    -- Detect runtime
    if tool.file_exists(app_dir .. "/app.lua") then
        info.runtime = "lua"
        info.entry = "app.lua"
    elseif tool.file_exists(app_dir .. "/app.js") then
        info.runtime = "js"
        info.entry = "app.js"
    else
        tool.stderr("hull deploy: no app.lua or app.js found in " .. app_dir .. "\n")
        tool.exit(1)
    end

    -- Try to extract manifest (Lua only — JS needs C runtime)
    if info.runtime == "lua" then
        local entry_path = app_dir .. "/app.lua"
        local chunk = tool.loadfile(entry_path)
        if chunk then
            local ok, err = pcall(chunk)
            if not ok then
                tool.stderr("hull deploy: warning: manifest extraction failed: " .. tostring(err) .. "\n")
            end
            if ok then
                local m = app.get_manifest()
                if m then
                    info.manifest = m
                    if m.env then
                        for _, v in ipairs(m.env) do
                            info.env_vars[#info.env_vars + 1] = v
                        end
                    end
                    if m.hosts then
                        for _, h in ipairs(m.hosts) do
                            info.hosts[#info.hosts + 1] = h
                        end
                    end
                end
            end
        end
    end

    -- Scan directories
    info.has_migrations = count_files(app_dir .. "/migrations", "*.sql") > 0
    info.has_static     = dir_exists(app_dir .. "/static")
    info.has_compute    = count_files(app_dir .. "/compute", "*.wasm") > 0
    info.has_shaders    = count_files(app_dir .. "/shaders", "*.wgsl") > 0
    info.has_signature  = tool.file_exists(app_dir .. "/package.sig")
    info.has_database   = info.has_migrations

    return info
end

-- ── Dockerfile generator ─────────────────────────────────────────────

local function gen_dockerfile(opts, info)
    local lines = {}
    local function add(s) lines[#lines + 1] = s end

    add("# Generated by hull deploy — do not edit")
    add("")
    add("# ── Build stage ─────────────────────────────────────────────────────")
    add("FROM debian:bookworm-slim AS build")
    add("")
    add("RUN apt-get update && apt-get install -y --no-install-recommends \\")
    add("    gcc libc6-dev make ca-certificates \\")
    add("    && rm -rf /var/lib/apt/lists/*")
    add("")
    add("COPY hull /usr/local/bin/hull")
    add("")
    add("# ── App build ───────────────────────────────────────────────────────")
    add("WORKDIR /src")
    add("COPY . .")
    add("RUN hull build . --output /app")

    if opts.sign then
        add("")
        add("COPY app.pub /src/app.pub")
        add("RUN hull verify . --developer-key /src/app.pub")
    end

    add("")
    add("# ── Runtime ─────────────────────────────────────────────────────────")
    if opts.distroless then
        add("FROM gcr.io/distroless/static-debian12")
    else
        add("FROM scratch")
    end

    add("")
    add("COPY --from=build /app /app")

    -- CA bundle if app makes outbound HTTPS
    if #info.hosts > 0 then
        add("COPY --from=build /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/")
    end

    -- ENV declarations from manifest
    if #info.env_vars > 0 then
        add("")
        for _, v in ipairs(info.env_vars) do
            add("ENV " .. v .. '=""')
        end
    end

    add("")
    add("EXPOSE " .. opts.port)

    if info.has_database then
        add("VOLUME [\"/data\"]")
    end

    add("")
    if info.has_database then
        add('ENTRYPOINT ["/app"]')
        add('CMD ["-p", "' .. opts.port .. '", "-d", "/data/data.db"]')
    else
        add('ENTRYPOINT ["/app"]')
        add('CMD ["-p", "' .. opts.port .. '"]')
    end

    add("")
    return table.concat(lines, "\n")
end

local function gen_dockerignore(opts, info)
    local lines = {
        "# Generated by hull deploy",
        "data.db*",
        "*.key",
        ".hull/",
        "build/",
        "*.sig",
        ".git/",
    }

    if tool.file_exists(opts.app_dir .. "/deploy") then
        lines[#lines + 1] = "deploy/"
    end

    lines[#lines + 1] = ""
    return table.concat(lines, "\n")
end

-- ── systemd generator ────────────────────────────────────────────────

local function gen_systemd_service(opts, info)
    local lines = {}
    local function add(s) lines[#lines + 1] = s end

    add("[Unit]")
    add("Description=Hull application: " .. opts.name)
    add("After=network-online.target")
    add("Wants=network-online.target")
    add("")
    add("[Service]")
    add("Type=simple")
    add("User=" .. opts.user)
    add("Group=" .. opts.user)

    local exec = opts.install_dir .. "/app -p " .. opts.port
    if info.has_database then
        exec = exec .. " -d " .. opts.data_dir .. "/data.db"
    end
    add("ExecStart=" .. exec)
    add("WorkingDirectory=" .. opts.install_dir)

    add("")
    add("# Secrets")
    add("EnvironmentFile=-" .. opts.install_dir .. "/.env")

    add("")
    add("# Security hardening (defense-in-depth with hull's own sandbox)")
    add("NoNewPrivileges=yes")
    add("ProtectSystem=strict")
    add("ProtectHome=yes")
    add("PrivateTmp=yes")
    add("PrivateDevices=yes")
    add("ProtectKernelTunables=yes")
    add("ProtectKernelModules=yes")
    add("ProtectControlGroups=yes")
    add("RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX")
    add("RestrictNamespaces=yes")
    add("RestrictRealtime=yes")
    add("RestrictSUIDSGID=yes")
    add("LockPersonality=yes")
    add("SystemCallFilter=@system-service")
    add("SystemCallArchitectures=native")
    add("MemoryDenyWriteExecute=yes")

    if info.has_database then
        add("ReadWritePaths=" .. opts.data_dir)
    end

    add("")
    add("Restart=on-failure")
    add("RestartSec=5")
    add("StandardOutput=journal")
    add("StandardError=journal")
    add("SyslogIdentifier=hull-" .. opts.name)

    add("")
    add("[Install]")
    add("WantedBy=multi-user.target")
    add("")

    return table.concat(lines, "\n")
end

local function gen_systemd_install(opts, info)
    local lines = {}
    local function add(s) lines[#lines + 1] = s end

    add("#!/bin/sh")
    add("# Installation commands for " .. opts.name)
    add("# Review and run manually.")
    add("set -e")
    add("")
    add('echo "Creating user and directories..."')
    add('echo "  sudo useradd --system --no-create-home ' .. opts.user .. '"')
    add('echo "  sudo mkdir -p ' .. opts.install_dir .. " " .. opts.data_dir .. '"')
    add('echo "  sudo cp app ' .. opts.install_dir .. '/app"')
    add('echo "  sudo chown -R ' .. opts.user .. ":" .. opts.user .. " " .. opts.data_dir .. '"')
    add("")
    add('echo ""')
    add('echo "Installing service..."')
    add('echo "  sudo cp deploy/' .. opts.name .. '.service /etc/systemd/system/"')
    add('echo "  sudo systemctl daemon-reload"')
    add('echo "  sudo systemctl enable --now ' .. opts.name .. '"')

    if #info.env_vars > 0 then
        add("")
        add('echo ""')
        add('echo "Env vars (create ' .. opts.install_dir .. '/.env):"')
        for _, v in ipairs(info.env_vars) do
            add('echo "  ' .. v .. '="')
        end
    end

    add("")
    return table.concat(lines, "\n")
end

-- ── fly.toml generator ───────────────────────────────────────────────

local function gen_fly_toml(opts, info)
    local lines = {}
    local function add(s) lines[#lines + 1] = s end

    add('app = "' .. opts.name .. '"')
    add('primary_region = "' .. opts.region .. '"')

    add("")
    add("[build]")
    add('  dockerfile = "Dockerfile"')

    if #info.env_vars > 0 then
        add("")
        add("[env]")
        add("  # Set secrets via: fly secrets set KEY=value")
        for _, v in ipairs(info.env_vars) do
            add("  # " .. v .. ' = ""')
        end
    end

    add("")
    add("[http_service]")
    add("  internal_port = " .. opts.port)
    add("  force_https = true")
    add('  auto_stop_machines = "stop"')
    add("  auto_start_machines = true")
    add("  min_machines_running = 0")

    add("")
    add("  [http_service.concurrency]")
    add('    type = "requests"')
    add("    hard_limit = 250")
    add("    soft_limit = 200")

    add("")
    add("[[vm]]")
    add('  memory = "' .. opts.memory .. 'mb"')
    add('  cpu_kind = "shared"')
    add("  cpus = 1")

    if info.has_database then
        add("")
        add("[mounts]")
        add('  source = "hull_data"')
        add('  destination = "/data"')
    end

    add("")
    return table.concat(lines, "\n")
end

-- ── File writing ─────────────────────────────────────────────────────

local function write_file(path, data, opts)
    if not opts.force and tool.file_exists(path) then
        tool.stderr("  skip: " .. path .. " (exists, use --force to overwrite)\n")
        return false
    end
    tool.write_file(path, data)
    print("  wrote: " .. path)
    return true
end

-- ── Target dispatchers ───────────────────────────────────────────────

local function deploy_dockerfile(opts, info)
    local out_dir = opts.output
    write_file(out_dir .. "/Dockerfile", gen_dockerfile(opts, info), opts)
    write_file(out_dir .. "/.dockerignore", gen_dockerignore(opts, info), opts)
    print("")
    print("Next steps:")
    print("  1. Copy the hull binary into " .. out_dir .. "/")
    print("  2. docker build -t " .. opts.name .. " " .. out_dir)
    print("  3. docker run -p " .. opts.port .. ":" .. opts.port .. " " .. opts.name)
end

local function deploy_systemd(opts, info)
    local deploy_dir = opts.output .. "/deploy"
    tool.mkdir(deploy_dir)
    write_file(deploy_dir .. "/" .. opts.name .. ".service",
               gen_systemd_service(opts, info), opts)
    write_file(deploy_dir .. "/install.sh",
               gen_systemd_install(opts, info), opts)
    print("")
    print("Next steps:")
    print("  1. hull build " .. opts.app_dir .. " --output app")
    print("  2. Review deploy/install.sh")
    print("  3. sh deploy/install.sh")
end

local function deploy_fly(opts, info)
    local out_dir = opts.output
    write_file(out_dir .. "/fly.toml", gen_fly_toml(opts, info), opts)

    -- Generate Dockerfile too if missing
    if not tool.file_exists(out_dir .. "/Dockerfile") then
        write_file(out_dir .. "/Dockerfile", gen_dockerfile(opts, info), opts)
        write_file(out_dir .. "/.dockerignore", gen_dockerignore(opts, info), opts)
    end

    print("")
    print("Next steps:")
    print("  1. Copy the hull binary into " .. out_dir .. "/")
    print("  2. fly launch (or fly deploy)")
end

-- ── Main ─────────────────────────────────────────────────────────────

local function main()
    local opts = parse_args()
    local info = introspect_app(opts.app_dir)

    print("hull deploy: generating " .. opts.target .. " config for " .. opts.name)
    print("")

    local targets = {
        dockerfile = deploy_dockerfile,
        systemd    = deploy_systemd,
        fly        = deploy_fly,
    }

    targets[opts.target](opts, info)
end

-- The tool dispatcher (src/hull/tool.c) invokes the returned main() only when
-- this module is the entry command it was asked to run. A module that is
-- require()'d as a dependency (e.g. by an app during manifest extraction in
-- the tool VM) hands its main() back but is never called, so it can't run
-- against the wrong argv.
return main
