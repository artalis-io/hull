--
-- hull.exec_lint - warnings about risky declared commands (`exec` section,
-- docs/design/exec_broker.md §3).
--
-- A WARN, never a boundary: the broker enforces the shapes whatever this
-- says. It points at the declarations most likely to give back what the
-- sandbox took away - a general-purpose program, a slot whose shape admits
-- almost anything, a slot that may start with '-', a binary outside the
-- system directories, a stream command (docs/design/exec_stream.md §7)
-- with no time limit or one past exec.run's 60 s, or with no total on an
-- output - so the author sees them at `hull build` / `hull check` time
-- rather than in a review.
--
-- lint(manifest) -> list of { cmd = name, msg = text }, sorted by command.
-- report(manifest, prefix) prints them to stderr and returns the count.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- Programs that run other programs, code, or arbitrary I/O named by an
-- argument (§1): a declared shape bounds their arguments, not what they do.
local GENERAL = {}
for name in ([[
sh bash dash zsh ksh mksh csh tcsh fish ash busybox toybox
env python perl ruby node nodejs deno bun lua luajit php tclsh wish
awk gawk mawk nawk sed find xargs parallel make
ssh scp sftp rsync curl wget nc ncat netcat socat telnet ftp
sudo doas su pkexec runuser setpriv capsh
systemd-run nsenter unshare chroot setsid nohup timeout watch nice ionice
strace ltrace gdb lldb
git tar docker podman kubectl crictl ctr
vi vim nvim emacs nano less more man
]]):gmatch("%S+") do GENERAL[name] = true end

-- python3, python3.12, perl5.36, ruby3.2, lua5.4, php8 -> their family.
local function family(base)
    local stem = base:match("^(%a+)[%d.]+$")
    if stem and GENERAL[stem] then return stem end
    return base
end

local SYSTEM_DIRS = { "/usr/", "/bin/", "/sbin/", "/opt/" }

local function in_system_dir(path)
    for _, d in ipairs(SYSTEM_DIRS) do
        if path:sub(1, #d) == d then return true end
    end
    return false
end

local function is_slot(a) return type(a) == "table" end

-- A manifest field in either runtime's spelling (Lua snake_case, JS camelCase).
local function field(c, snake, camel)
    local v = c[snake]
    if v == nil then v = c[camel] end
    return v
end

-- A stream command may run as long as it declares, or with no time limit
-- at all, and write any total: the declaration is the bound, so say so.
local function lint_stream(c, warn)
    local t = field(c, "timeout_ms", "timeoutMs")
    if t == 0 then
        warn("stream command with no time limit (timeout_ms = 0): it runs until the "
             .. "app closes it, its request goes, or Hull stops")
    elseif type(t) == "number" and t > 60000 then
        warn(string.format("stream command with a time limit of %d s, past exec.run's "
             .. "60 s", t // 1000))
    end
    local open = {}
    if field(c, "stdout_max", "stdoutMax") == nil then open[#open + 1] = "stdout" end
    if field(c, "stderr_max", "stderrMax") == nil then open[#open + 1] = "stderr" end
    if #open > 0 then
        warn("stream command with no total on " .. table.concat(open, " or ")
             .. ": it may write without bound; declare "
             .. (#open == 2 and "stdout_max and stderr_max" or open[1] .. "_max"))
    end
end

function M.lint(m)
    local out = {}
    local exec = type(m) == "table" and m.exec or nil
    if type(exec) ~= "table" then return out end
    local names = {}
    for name in pairs(exec) do
        if type(name) == "string" then names[#names + 1] = name end
    end
    table.sort(names)
    for _, name in ipairs(names) do
        local c = exec[name]
        local function warn(msg) out[#out + 1] = { cmd = name, msg = msg } end
        if type(c) == "table" then
            local path = type(c.path) == "string" and c.path or ""
            local base = path:match("([^/]+)$") or path
            if GENERAL[family(base)] then
                warn(base .. " is a general-purpose program: a declared shape bounds "
                     .. "its arguments, not what it runs or reaches")
            end
            if path ~= "" and not in_system_dir(path) then
                warn(path .. " is outside /usr, /bin, /sbin and /opt")
            end
            if c.stream == true then lint_stream(c, warn) end
            local argv = type(c.argv) == "table" and c.argv or {}
            local seen_fixed, seen_dashdash, suggested = false, false, false
            for i, a in ipairs(argv) do
                if is_slot(a) then
                    local enum = a.enum
                    local dash = a.allow_dash or a.allowDash
                    if dash then
                        warn("argv " .. i .. " may start with '-': the value can be "
                             .. "read as an option (--output=..., --host=...)")
                    end
                    if enum == nil and a.chars == nil then
                        warn("argv " .. i .. " has no chars or enum: it takes any of "
                             .. "[A-Za-z0-9._-] up to its max; narrow it")
                    end
                    if enum == nil and seen_fixed and not seen_dashdash
                       and not suggested then
                        suggested = true
                        warn("argv " .. i .. " follows fixed arguments with no \"--\" "
                             .. "before it; put \"--\" first so the program reads it "
                             .. "as an operand")
                    end
                else
                    seen_fixed = true
                    if a == "--" then seen_dashdash = true end
                end
            end
        end
    end
    return out
end

function M.report(m, prefix)
    local list = M.lint(m)
    for _, w in ipairs(list) do
        tool.stderr(prefix .. ": WARN exec." .. tool.escape(w.cmd) .. ": "
                    .. tool.escape(w.msg) .. "\n")
    end
    return #list
end

return M
