--
-- hull.types - install the TypeScript declarations for Hull's JavaScript API
--
-- Usage: hull types [app_dir] [-o DIR] [--force] [--check]
--
-- Writes the declarations embedded in this hull binary (types/hull,
-- types/hull-test in the repository) under <app_dir>/types/ (or -o DIR), so an
-- editor or `tsc --noEmit` can type-check the app's JavaScript (checkJs) or
-- TypeScript. Hull itself never reads them.
--
-- The files Hull wrote are recorded, with their SHA-256, in
-- <out>/hull/.hull-types. A re-run (after `hull update`) replaces a recorded
-- file that is unchanged and removes one this version no longer ships; a file
-- that was edited, or that Hull did not write, is a conflict: nothing is
-- written and the command fails unless --force. --check writes nothing and
-- fails when the installed declarations differ from this binary's.
--
-- A tsconfig.json (and tests/tsconfig.json) is written only where none
-- exists; an existing one is never touched.
--
-- Design: docs/typescript_design.md §2.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local RECORD = "hull/.hull-types"

local function usage()
    tool.stdout([[
Usage: hull types [app_dir] [-o DIR] [--force] [--check]

Install the TypeScript declarations for Hull's JavaScript API into
<app_dir>/types (or DIR), plus a tsconfig.json when the app has none.

  -o, --output DIR   write the declarations to DIR instead of <app_dir>/types
  --force            replace files that were edited or not written by hull
  --check            write nothing; exit 1 if the installed declarations differ
                     from this hull's

Type-check the app with an installed TypeScript:  tsc --noEmit -p .
]])
end

local function fail(msg)
    tool.stderr("hull types: " .. tool.escape(msg) .. "\n")
    tool.exit(1)
end

local function parse_args()
    local opts = { dir = nil }
    local i = 1
    while arg[i] do
        local a = arg[i]
        if a == "-h" or a == "--help" then opts.help = true
        elseif a == "--force" then opts.force = true
        elseif a == "--check" then opts.check = true
        elseif a == "-o" or a == "--output" then
            i = i + 1
            if not arg[i] then fail(a .. " needs a directory") end
            opts.out = arg[i]
        elseif a:sub(1, 9) == "--output=" then
            opts.out = a:sub(10)
        elseif a:sub(1, 1) == "-" then
            fail("unknown option '" .. a .. "' (see hull types --help)")
        elseif opts.dir then
            fail("one app directory at most")
        else
            opts.dir = a
        end
        i = i + 1
    end
    opts.dir = opts.dir or "."
    return opts
end

local function join(a, b)
    if a == "." then return b end
    return a .. "/" .. b
end

local function parent(path)
    return path:match("^(.*)/[^/]*$")
end

-- A name hull types could have written: "hull/..." or "hull-test/...", plain
-- components (no empty, ".", ".." or dot-leading one, no backslash), ending
-- in .d.ts or .md - the shape of every declaration file any version embeds.
-- The record is a file in the app tree, so its names are repo content: a
-- planted "../../x" entry made the stale-file removal delete outside types/
-- (audit 46).
local function shipped_name(name)
    if type(name) ~= "string" or #name > 512 then return false end
    if not (name:match("^hull/") or name:match("^hull%-test/")) then return false end
    if not (name:match("%.d%.ts$") or name:match("%.md$")) then return false end
    if name:find("//", 1, true) or name:sub(-1) == "/" then return false end
    for seg in name:gmatch("[^/]+") do
        if not seg:match("^[%w_%-][%w_.%-]*$") then return false end
    end
    return true
end

-- The first path from base down to base/rel (each component, the leaf
-- included) that is a symbolic link, or nil. A linked directory below types/
-- (types/hull/modules -> elsewhere) sent writes and removals outside it
-- (audit 46).
local function link_below(base, rel)
    local acc = base
    for seg in rel:gmatch("[^/]+") do
        acc = join(acc, seg)
        if tool.is_symlink(acc) then return acc end
    end
    return nil
end

-- The record: "version <v>" then "<sha256> <relative path>" per file.
local function read_record(out)
    local path = join(out, RECORD)
    if not tool.file_exists(path) then return nil end
    local body = tool.read_file(path)
    if not body then return nil end
    local rec = { files = {} }
    for line in body:gmatch("[^\n]+") do
        local v = line:match("^version (%S+)$")
        if v then rec.version = v
        else
            local h, p = line:match("^(%x+) (.+)$")
            if h then rec.files[p] = h end
        end
    end
    return rec
end

local function write_record(out, version, hashes)
    local names = {}
    for p in pairs(hashes) do names[#names + 1] = p end
    table.sort(names)
    local lines = { "version " .. version }
    for _, p in ipairs(names) do lines[#lines + 1] = hashes[p] .. " " .. p end
    return tool.write_file(join(out, RECORD), table.concat(lines, "\n") .. "\n")
end

local TSCONFIG = [[{
  "compilerOptions": {
    "noEmit": true,
    "allowJs": true,
    "checkJs": true,
    "strict": true,
    "noImplicitAny": false,
    "strictNullChecks": false,
    "lib": ["es2023"],
    "target": "es2023",
    "module": "esnext",
    "moduleResolution": "bundler",
    "allowImportingTsExtensions": true,
    "types": []
  },
  "include": ["**/*.js", "**/*.ts", "types/hull/**/*.d.ts"],
  "exclude": ["tests/**", "static/**", "node_modules/**"]
}
]]

local TSCONFIG_TESTS = [[{
  "extends": "../tsconfig.json",
  "include": ["**/*.js", "**/*.ts", "../types/hull/**/*.d.ts", "../types/hull-test/**/*.d.ts"],
  "exclude": []
}
]]

local function main()
    local opts = parse_args()
    if opts.help then usage() return end

    local kind = tool.path_kind(opts.dir)
    if kind ~= "dir" then fail("'" .. opts.dir .. "' is not a directory") end
    local out = opts.out or join(opts.dir, "types")
    if tool.is_symlink(out) then fail("'" .. out .. "' is a symbolic link") end

    local files, version = tool.types_files()
    if #files == 0 then fail("this hull binary embeds no TypeScript declarations") end

    local record_link = link_below(out, RECORD)
    if record_link then fail("'" .. record_link .. "' is a symbolic link") end
    local rec = read_record(out) or { files = {} }
    local embedded = {}
    local stale, conflicts, writes = {}, {}, {}
    for _, f in ipairs(files) do
        embedded[f.name] = true
        local path = join(out, f.name)
        local link = link_below(out, f.name)
        if link then
            conflicts[#conflicts + 1] = f.name .. " (" .. link .. " is a symbolic link)"
        elseif not tool.file_exists(path) then
            writes[#writes + 1] = f
        elseif tool.read_file(path) ~= f.body then
            local mine = rec.files[f.name] and rec.files[f.name] == tool.sha256_file(path)
            if mine or opts.force then
                writes[#writes + 1] = f
            else
                conflicts[#conflicts + 1] = f.name
            end
        end
    end
    -- Files an earlier hull wrote that this one no longer ships.
    -- Only names hull types could have written are acted on; any other
    -- entry is ignored (and reported by --check).
    local ignored = {}
    for name, hash in pairs(rec.files) do
        if not shipped_name(name) then
            ignored[#ignored + 1] = name
        elseif not embedded[name] then
            local path = join(out, name)
            local link = link_below(out, name)
            if link then
                conflicts[#conflicts + 1] = name .. " (" .. link .. " is a symbolic link)"
            elseif tool.file_exists(path) and tool.sha256_file(path) == hash then
                stale[#stale + 1] = name
            end
        end
    end
    table.sort(stale)
    table.sort(conflicts)
    table.sort(ignored)

    if opts.check then
        local bad = #writes + #stale + #conflicts + #ignored
        if rec.version ~= version then bad = bad + 1 end
        if bad == 0 then
            print("hull types: " .. out .. " matches hull " .. version)
            return
        end
        tool.stderr("hull types: " .. tool.escape(out) .. " is not hull " .. version ..
                    "'s declarations (installed: " .. tool.escape(rec.version or "none") .. ")\n")
        for _, f in ipairs(writes) do tool.stderr("  differs   " .. tool.escape(f.name) .. "\n") end
        for _, n in ipairs(conflicts) do tool.stderr("  edited    " .. tool.escape(n) .. "\n") end
        for _, n in ipairs(stale) do tool.stderr("  stale     " .. tool.escape(n) .. "\n") end
        for _, n in ipairs(ignored) do
            tool.stderr("  ignored   " .. tool.escape(n) .. " (a record entry hull types never writes)\n")
        end
        tool.stderr("Run `hull types` to update them.\n")
        tool.exit(1)
    end

    if #conflicts > 0 then
        tool.stderr("hull types: these files were edited or not written by hull; nothing was written:\n")
        for _, n in ipairs(conflicts) do tool.stderr("  " .. tool.escape(join(out, n)) .. "\n") end
        tool.stderr("Re-run with --force to replace them.\n")
        tool.exit(1)
    end

    -- Record every embedded file, written now or already identical.
    local hashes = {}
    for _, f in ipairs(writes) do
        local dir = parent(join(out, f.name))
        if dir and not tool.file_exists(dir) and not tool.mkdir(dir) then
            fail("cannot create " .. dir)
        end
        if not tool.write_file(join(out, f.name), f.body) then
            fail("cannot write " .. join(out, f.name))
        end
    end
    for _, f in ipairs(files) do
        local h = tool.sha256_file(join(out, f.name))
        if not h then fail("cannot read back " .. join(out, f.name)) end
        hashes[f.name] = h
    end
    local removed, unremoved = 0, {}
    for _, n in ipairs(stale) do
        if tool.remove_file(join(out, n)) then removed = removed + 1
        else unremoved[#unremoved + 1] = n end
    end
    if not write_record(out, version, hashes) then fail("cannot write " .. join(out, RECORD)) end

    print("hull types: " .. out .. " (hull " .. version .. ")")
    print(string.format("  %d written, %d unchanged, %d removed",
        #writes, #files - #writes, removed))
    for _, n in ipairs(unremoved) do
        tool.stderr("hull types: could not remove " .. tool.escape(join(out, n)) .. "\n")
    end
    for _, n in ipairs(ignored) do
        tool.stderr("hull types: ignored record entry " .. tool.escape(n) ..
                    " (a name hull types never writes)\n")
    end

    -- tsconfig.json only where none exists, and only for the default layout
    -- (its include paths assume <app_dir>/types).
    if opts.out then
        print("  -o given: write your own tsconfig.json pointing at " .. out)
        return
    end
    local tsconfig = join(opts.dir, "tsconfig.json")
    if tool.is_symlink(tsconfig) then
        print("  tsconfig.json is a symbolic link (not written)")
    elseif tool.file_exists(tsconfig) then
        print("  tsconfig.json exists (unchanged); it needs \"types/hull/**/*.d.ts\" in include")
    elseif tool.write_file(tsconfig, TSCONFIG) then
        print("  created tsconfig.json")
    end
    local tests = join(opts.dir, "tests")
    if tool.is_symlink(tests) then
        print("  tests/ is a symbolic link: tests/tsconfig.json not written")
    elseif tool.path_kind(tests) == "dir" then
        local tt = join(tests, "tsconfig.json")
        if not tool.is_symlink(tt) and not tool.file_exists(tt)
           and tool.write_file(tt, TSCONFIG_TESTS) then
            print("  created tests/tsconfig.json")
        end
    end
    print("")
    print("Type-check with an installed TypeScript:  tsc --noEmit -p " ..
          (opts.dir == "." and "." or opts.dir) ..
          ((tool.path_kind(tests) == "dir" and not tool.is_symlink(tests)) and ("  (tests: -p " .. join(opts.dir, "tests") .. ")") or ""))
end

return main
