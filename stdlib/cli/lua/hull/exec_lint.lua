--
-- hull.exec_lint - warnings about risky declared commands (`exec` section,
-- docs/design/exec_broker.md §3).
--
-- A WARN, never a boundary: the broker enforces the shapes whatever this
-- says. It points at the declarations most likely to give back what the
-- sandbox took away - a general-purpose program, a slot whose shape admits
-- almost anything, a slot that may start with '-', a binary outside the
-- system directories - so the author sees them at `hull build` /
-- `hull check` time rather than in a review.
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
            -- The v0.26.0 spellings: taken until 0.28.0, named here with
            -- their replacement (docs/design/exec_broker.md §3).
            if c.fs ~= nil then
                warn("fs is the v0.26.0 spelling of host = { read, write } (absolute "
                     .. "paths); rename it - it is refused from 0.28.0. An app-relative "
                     .. "grant goes in manifest = { fs = { ... } }")
            end
            if c.unix_sockets ~= nil or c.unixSockets ~= nil then
                warn("unix_sockets is the v0.26.0 spelling of host.unix_sockets; "
                     .. "rename it - it is refused from 0.28.0")
            end
            local path = type(c.path) == "string" and c.path or ""
            local base = path:match("([^/]+)$") or path
            if GENERAL[family(base)] then
                warn(base .. " is a general-purpose program: a declared shape bounds "
                     .. "its arguments, not what it runs or reaches")
            end
            if path ~= "" and not in_system_dir(path) then
                warn(path .. " is outside /usr, /bin, /sbin and /opt")
            end
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
