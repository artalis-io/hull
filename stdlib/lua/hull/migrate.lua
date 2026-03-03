--
-- hull.migrate — Migration scaffolding tool
--
-- Handles the 'new' subcommand: creates numbered .sql migration files.
-- The 'run' and 'status' subcommands are handled in C (commands/migrate.c).
--
-- Usage: hull migrate new <name>
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

-- ── Argument parsing ─────────────────────────────────────────────────

local function parse_args()
    local opts = {
        subcmd = nil,
        name = nil,
        app_dir = ".",
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "new" then
            opts.subcmd = "new"
        elseif a:sub(1, 1) ~= "-" then
            if opts.subcmd == "new" and not opts.name then
                opts.name = a
            else
                opts.app_dir = a
            end
        end
        i = i + 1
    end

    return opts
end

-- ── Helpers ──────────────────────────────────────────────────────────

local function find_next_sequence(dir)
    local files = tool.find_files(dir, "*.sql")
    local max_seq = 0

    for _, path in ipairs(files) do
        local basename = path:match("([^/]+)$")
        local seq = basename and tonumber(basename:match("^(%d+)"))
        if seq and seq > max_seq then
            max_seq = seq
        end
    end

    return max_seq + 1
end

local function validate_name(name)
    return name:match("^[a-zA-Z0-9_]+$") ~= nil
end

-- ── Main ─────────────────────────────────────────────────────────────

local function main()
    local opts = parse_args()

    if opts.subcmd ~= "new" then
        tool.stderr("hull migrate: unknown subcommand\n")
        tool.exit(1)
    end

    if not opts.name then
        tool.stderr("Usage: hull migrate new <name>\n")
        tool.stderr("  name: alphanumeric + underscores (e.g. add_users)\n")
        tool.exit(1)
    end

    if not validate_name(opts.name) then
        tool.stderr("hull migrate: invalid name '" .. opts.name ..
            "' (use alphanumeric + underscores)\n")
        tool.exit(1)
    end

    local migrations_dir = opts.app_dir .. "/migrations"

    -- Create migrations/ directory if it doesn't exist
    if not tool.file_exists(migrations_dir) then
        tool.mkdir(migrations_dir)
    end

    -- Find next sequence number
    local seq = find_next_sequence(migrations_dir)
    local filename = string.format("%03d_%s.sql", seq, opts.name)
    local filepath = migrations_dir .. "/" .. filename

    -- Write migration file with header comment
    local content = string.format("-- Migration: %s\n\n", filename)
    tool.write_file(filepath, content)

    print("hull migrate: created " .. filepath)
end

main()
