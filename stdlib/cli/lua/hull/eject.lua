--
-- hull.eject — Export standalone Makefile project
--
-- Usage: hull eject [app_dir] [-o output_dir]
--
-- Generates a self-contained project that builds without hull.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local fcompose = require("hull.feature_compose")

-- ── Argument parsing ─────────────────────────────────────────────────

local function parse_args()
    local opts = {
        app_dir = ".",
        output = nil,
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "-o" or a == "--output" then
            i = i + 1
            opts.output = arg[i]
        elseif a:sub(1, 1) ~= "-" then
            opts.app_dir = a
        end
        i = i + 1
    end

    if not opts.output then
        opts.output = opts.app_dir .. "_ejected"
    end

    return opts
end

-- ── Templates ────────────────────────────────────────────────────────

-- Generate the standalone Makefile.
--
--   rt == nil  → cosmo dual base (both runtimes embedded in the fat-APE
--                platform lib); links platform/libhull_platform.a. When the
--                app's runtime is known (`cosmo_rt`), also compiles + links a
--                stdlib-only app_stdlib_registry.o BEFORE the archive: the
--                base's strong hl_stdlib_feature_entries() is archive-shadowed
--                by the weak default, so a plain link would boot with an empty
--                runtime VFS ("module not found: hull.json"). Mirrors the
--                `hull build` cosmo path.
--   rt table   → native runtime-less base; also compiles the generated
--                app_feature_registry.o and whole-archive-links the composed
--                runtime archive. rt = { name = "lua"|"js", darwin = bool,
--                fcompose = <module> }.
--
-- `out_name` is the output binary name. It must NOT be "app": the embedded
-- source lives under app/, and a target named `app` collides with that
-- directory (make treats the up-to-date dir as the target and never links).
-- Callers guarantee out_name ~= "app".
local function gen_makefile(rt, out_name, cosmo_rt)
    local sub = out_name:gsub("%%", "%%%%")
    if not rt then
        -- The stdlib override object is ordered BEFORE the archive in the link
        -- deps so its strong hl_stdlib_feature_entries() wins over the base's
        -- weak default (strong-over-weak). `$^` preserves prereq order.
        local reg_rule, reg_obj = "", ""
        if cosmo_rt then
            reg_obj = " build/app_stdlib_registry.o"
            reg_rule = "\nbuild/app_stdlib_registry.o: src/app_stdlib_registry.c | build"
                .. "\n\t$(CC) $(CFLAGS) -Isrc -c -o $@ $<\n"
        end
        return ([[CC      ?= cosmocc
CFLAGS  := -std=c11 -O2 -w
OUTPUT  ?= __OUTPUT__

APP_FILES := $(shell find app -name '*.lua' -o -name '*.js')

.PHONY: all clean

all: $(OUTPUT)

build:
	mkdir -p build

build/app_registry.c: $(APP_FILES) | build
	sh scripts/gen_registry.sh app > $@

build/app_registry.o: build/app_registry.c | build
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

build/app_main.o: src/app_main.c | build
	$(CC) $(CFLAGS) -c -o $@ $<
__REG_RULE__
$(OUTPUT): build/app_main.o build/app_registry.o__REG_OBJ__ platform/libhull_platform.a
	$(CC) -o $@ $^ -lm -lpthread

clean:
	rm -rf build $(OUTPUT)
]]):gsub("__REG_RULE__", (reg_rule:gsub("%%", "%%%%")))
   :gsub("__REG_OBJ__", (reg_obj:gsub("%%", "%%%%")))
   :gsub("__OUTPUT__", sub)
    end

    -- Native runtime-less base: link the platform lib + the whole-archived
    -- mandatory feature set (runtime + http/wasm/sqlite-rt/image/tls/keel as the
    -- app needs them, in the plan's order). Archives that reference base / Keel
    -- symbols (runtime, tls, keel) need the GNU-ld --start-group with the
    -- platform lib; ld64 rescans natively and rejects the flag, so darwin uses a
    -- plain platform lib + -force_load. The default CC is the native `cc` (the
    -- extracted .a is native single-arch; cosmocc would reject its object format).
    local parts = {}
    for _, d in ipairs(rt.plan) do
        for _, f in ipairs(rt.fcompose.whole_archive_flags("platform/" .. d.lib, rt.darwin)) do
            parts[#parts + 1] = f
        end
    end
    local waf = table.concat(parts, " ")
    local link_libs
    if rt.darwin or not rt.base_group then
        link_libs = "platform/libhull_platform.a " .. waf
    else
        link_libs = "-Wl,--start-group platform/libhull_platform.a " .. waf
                    .. " -Wl,--end-group"
    end
    return ([[CC      ?= cc
CFLAGS  := -std=c11 -O2 -w
OUTPUT  ?= __OUTPUT__

APP_FILES := $(shell find app -name '*.lua' -o -name '*.js')

.PHONY: all clean

all: $(OUTPUT)

build:
	mkdir -p build

build/app_registry.c: $(APP_FILES) | build
	sh scripts/gen_registry.sh app > $@

build/app_registry.o: build/app_registry.c | build
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

build/app_feature_registry.o: src/app_feature_registry.c | build
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

build/app_main.o: src/app_main.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUTPUT): build/app_main.o build/app_registry.o build/app_feature_registry.o
	$(CC) -o $@ build/app_main.o build/app_registry.o build/app_feature_registry.o __LINK_LIBS__ -lm -lpthread

clean:
	rm -rf build $(OUTPUT)
]]):gsub("__LINK_LIBS__", (link_libs:gsub("%%", "%%%%"))):gsub("__OUTPUT__", sub)
end

local function gen_app_main()
    -- A produced app is an app-runner, not the hull CLI: hl_app_run runs THIS
    -- app (no subcommand dispatch), matching what `hull build` emits. Both
    -- symbols live in the platform lib; hl_app_run is the slim contract.
    return [[/* app_main.c — Thin entry point (calls hl_app_run from platform lib) */
#include "entry.h"
extern int hl_app_run(int argc, char **argv);
int main(int argc, char **argv) { return hl_app_run(argc, argv); }
]]
end

local function gen_entry_h()
    return [[/* entry.h — App entry type definition */
#ifndef HL_ENTRY_H
#define HL_ENTRY_H

typedef struct {
    const char *name;
    const unsigned char *data;
    unsigned int len;
} HlEntry;

#endif /* HL_ENTRY_H */
]]
end

local function gen_registry_sh()
    return [[#!/bin/sh
# gen_registry.sh — Generate app_registry.c from app files
#
# Usage: sh scripts/gen_registry.sh <app_dir>
#
# Generates a C file with embedded app files as byte arrays,
# plus a registry table mapping module names to data.

APP_DIR="${1:-.}"

echo "/* Auto-generated by gen_registry.sh — do not edit */"
echo '#include "entry.h"'
echo ""

# Find all .lua and .js modules, sorted for deterministic output. Both
# runtimes are handled: a Lua entry is named "./name" (extension stripped),
# a JS entry "./name.js" (extension kept) — matching `hull build`'s registry.
FILES=$(find "$APP_DIR" \( -name '*.lua' -o -name '*.js' \) -not -path '*/.*' | sort)

# Emit byte arrays
for f in $FILES; do
    # Variable name: replace /. with _
    varname=$(echo "$f" | sed 's/[\/.]/_/g')
    echo "static const unsigned char ${varname}[] = {"

    # Use xxd if available, otherwise fall back to od
    if command -v xxd >/dev/null 2>&1; then
        xxd -i < "$f" | grep -v '^unsigned'
    else
        od -An -tx1 -v "$f" | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g; s/^/  /'
    fi

    echo "};"
    echo ""
done

# Emit registry table
echo '#include "entry.h"'
echo "const HlEntry hl_app_entries[] = {"

for f in $FILES; do
    varname=$(echo "$f" | sed 's/[\/.]/_/g')
    # Module name: strip app_dir prefix, add ./ prefix. Lua drops its .lua
    # suffix; JS keeps its .js suffix (the runtime resolves by full name).
    case "$f" in
        *.lua) modname=$(echo "$f" | sed "s|^${APP_DIR}/||; s|\.lua$||") ;;
        *)     modname=$(echo "$f" | sed "s|^${APP_DIR}/||") ;;
    esac
    echo "    { \"./${modname}\", ${varname}, sizeof(${varname}) },"
done

echo "    { 0, 0, 0 }"
echo "};"
]]
end

-- ── File utilities ───────────────────────────────────────────────────

local function write_file(path, data)
    return tool.write_file(path, data)
end

-- ── Main ─────────────────────────────────────────────────────────────

local function main()
    local opts = parse_args()

    -- Verify app_dir has files
    local files = tool.find_files(opts.app_dir, "*.lua")
    if not files or #files == 0 then
        tool.stderr("hull eject: no .lua files found in " .. opts.app_dir .. "\n")
        tool.exit(1)
    end

    -- Check output doesn't already exist
    if tool.file_exists(opts.output) then
        tool.stderr("hull eject: output directory '" .. opts.output .. "' already exists\n")
        tool.exit(1)
    end

    -- Find platform library.
    --
    -- Search order:
    --   1. Embedded blob — release binaries (built with EMBED_PLATFORM=1)
    --      ship the .a inside the hull binary. Extract to a tmpdir;
    --      tmpdir becomes the canonical search-path entry. End-users who
    --      installed via `gethull.dev/install.sh` end up here.
    --   2. dirname(hull_exe) — adjacent .a from `make platform` or a
    --      previous extraction.
    --   3. ./build/ and ../build/ — Hull source-tree conventional spots.
    local platform_lib = nil
    local platform_lib_arm = nil  -- aarch64 cosmo archive (if multi-arch)
    local hull_dir = ""
    if __hull_exe then
        hull_dir = __hull_exe:match("(.*/)" ) or ""
    end

    -- Try the embedded blob first. Both extractors return true/false; the
    -- ones that fit nothing (no embedded blob, wrong arch shape) are a
    -- no-op and we continue with the disk search.
    local extracted_dir = tool.tmpdir()
    local extracted_ok = false
    if tool.extract_platform_cosmo then
        extracted_ok = tool.extract_platform_cosmo(extracted_dir) or extracted_ok
    end
    if not extracted_ok and tool.extract_platform then
        extracted_ok = tool.extract_platform(extracted_dir) or extracted_ok
    end

    local search_paths = {
        extracted_dir .. "/",
        hull_dir,
        "build/",
        "../build/",
    }

    -- Check for multi-arch cosmo archives first
    for _, d in ipairs(search_paths) do
        local x86 = d .. "libhull_platform.x86_64-cosmo.a"
        local arm = d .. "libhull_platform.aarch64-cosmo.a"
        if tool.file_exists(x86) and tool.file_exists(arm) then
            platform_lib = x86
            platform_lib_arm = arm
            break
        end
    end

    -- Fallback to single-arch archive
    if not platform_lib then
        for _, d in ipairs(search_paths) do
            if tool.file_exists(d .. "libhull_platform.a") then
                platform_lib = d .. "libhull_platform.a"
                break
            end
        end
    end

    if not platform_lib then
        tool.stderr("hull eject: cannot find libhull_platform.a\n")
        tool.stderr("hint: this hull binary has no embedded platform library\n")
        tool.stderr("      and none was found in PATH-adjacent locations.\n")
        tool.stderr("      install a release binary from https://gethull.dev,\n")
        tool.stderr("      or run `make platform` in a hull source tree.\n")
        tool.exit(1)
    end

    -- Native runtime composition. The cosmo dual base (the multi-arch pair)
    -- embeds both runtimes and is self-sufficient. A native single-arch base is
    -- RUNTIME-LESS, so an ejected native app must ALSO bundle its one runtime
    -- feature archive + a generated app_feature_registry, exactly as
    -- `hull build` does (shared via hull.feature_compose). Without this the
    -- ejected binary links but has zero runtimes and an empty stdlib -- it
    -- cannot run any app.
    local rt_compose = nil
    local cosmo_rt = nil  -- app runtime for the cosmo dual base (stdlib override)
    if platform_lib_arm then
        -- Cosmo dual base: composition-exempt (both runtimes embedded), so no
        -- runtime archive to compose. But its strong hl_stdlib_feature_entries()
        -- is archive-shadowed by the weak default (see
        -- feature_compose.gen_app_registry_c), so - exactly as `hull build` does
        -- - emit a stdlib-only strong override as a regular object, else the
        -- ejected APE boots with an empty runtime VFS ("module not found:
        -- hull.json"). Requires knowing the app's one runtime.
        cosmo_rt = fcompose.detect_app_rt(opts.app_dir)
    end
    if not platform_lib_arm then
        local app_rt = fcompose.detect_app_rt(opts.app_dir)
        if app_rt then
            local plat = tool.platform_name and tool.platform_name() or nil
            -- Count compute/*.wasm so a WASM-backed db.udf (which declares no
            -- compute module, invisible to the resolver) still composes the wasm
            -- feature -- plan_mandatory's S2 signal, matching `hull build`.
            local compute_count = 0
            if tool.file_exists(opts.app_dir .. "/compute") then
                local wasm = tool.find_files(opts.app_dir .. "/compute", "*.wasm")
                if wasm then compute_count = #wasm end
            end
            -- Compose the SAME ordered mandatory-archive set `hull build` does
            -- (runtime + http/wasm/sqlite-rt/image/tls/keel as the app needs
            -- them) via the shared plan. Before this, eject composed ONLY runtime
            -- + http, so an ejected db/compute/image/HTTPS app failed to link
            -- (missing sqlite3_*/WAMR/mbedTLS symbols). One source of truth now.
            local plan = fcompose.plan_mandatory({
                app_dir = opts.app_dir, app_rt = app_rt, tmpdir = extracted_dir,
                hull_dir = hull_dir, plat = plat, platform_lib = platform_lib,
                compute_count = compute_count, with = opts.with, flavor = opts.flavor,
                with_list = {},
                on_fail = function(msg)
                    tool.stderr((msg:gsub("hull build:", "hull eject:")))
                    tool.exit(1)
                end,
            })
            local base_group = false
            for _, d in ipairs(plan.list) do
                if d.sets_base_group then base_group = true end
            end
            rt_compose = {
                name = app_rt,
                plan = plan.list,
                base_group = base_group,
                darwin = (plat and plat:sub(1, 6) == "darwin") or false,
                fcompose = fcompose,
            }
        end
    end

    -- Output binary name: the app dir's basename, sanitized. Never "app" --
    -- that collides with the embedded app/ source directory (make would treat
    -- the up-to-date dir as the target and skip the link).
    local out_name = opts.app_dir:gsub("/+$", ""):match("[^/]+$")
    if not out_name or out_name == "" or out_name == "." or out_name == ".." then
        out_name = "hull_app"
    end
    out_name = out_name:gsub("[^%w._-]", "_")
    if out_name == "app" then out_name = "app_bin" end

    -- Create output structure
    local dir = opts.output
    tool.mkdir(dir)
    tool.mkdir(dir .. "/src")
    tool.mkdir(dir .. "/app")
    tool.mkdir(dir .. "/platform")
    tool.mkdir(dir .. "/scripts")
    tool.mkdir(dir .. "/build")

    -- Write generated files
    write_file(dir .. "/Makefile", gen_makefile(rt_compose, out_name, cosmo_rt))
    write_file(dir .. "/src/app_main.c", gen_app_main())
    write_file(dir .. "/src/entry.h", gen_entry_h())
    write_file(dir .. "/scripts/gen_registry.sh", gen_registry_sh())

    -- Cosmo dual base: the stdlib-only strong override the Makefile compiles +
    -- links before the archive (see gen_makefile / gen_app_registry_c).
    if cosmo_rt then
        write_file(dir .. "/src/app_stdlib_registry.c",
                   fcompose.gen_app_registry_c(cosmo_rt, { factory = false }))
    end

    -- Native runtime feature: the generated registry (strong hooks for the
    -- app's one runtime) + the whole-archived runtime lib the Makefile links.
    if rt_compose then
        write_file(dir .. "/src/app_feature_registry.c",
                   fcompose.gen_app_registry_c(rt_compose.name))
        -- Copy every composed archive in the plan into platform/ under its
        -- canonical filename; the Makefile whole-archives each (see gen_makefile).
        for _, d in ipairs(rt_compose.plan) do
            tool.copy(d.path, dir .. "/platform/" .. d.lib)
        end
    end

    -- Copy platform library (multi-arch: x86_64 + .aarch64/ layout for cosmocc)
    if platform_lib_arm then
        tool.copy(platform_lib, dir .. "/platform/libhull_platform.a")
        tool.mkdir(dir .. "/platform/.aarch64")
        tool.copy(platform_lib_arm, dir .. "/platform/.aarch64/libhull_platform.a")
    else
        tool.copy(platform_lib, dir .. "/platform/libhull_platform.a")
    end

    -- Copy app files, preserving directory structure
    for _, path in ipairs(files) do
        local rel = path:sub(#opts.app_dir + 2)

        -- Path-traversal defense (mirrors verify.lua): a crafted app tree with a
        -- `..`-escaping or absolute relative path would otherwise write outside
        -- the output dir.
        if rel:find("%.%.") or rel:sub(1, 1) == "/" then
            tool.stderr("hull eject: skipping suspicious app path: " .. rel .. "\n")
            goto continue_files
        end

        local dest = dir .. "/app/" .. rel

        -- Never copy a file onto itself (tool.copy opens dest "wb" before reading
        -- src, so src==dest 0-bytes the source). Guards `hull eject . -o .`-style
        -- output/app-dir overlaps.
        if dest ~= path then
            -- Create parent directories
            local parent = dest:match("(.*/)")
            if parent then
                tool.mkdir(parent)
            end
            tool.copy(path, dest)
        end
        ::continue_files::
    end

    print("hull eject: created " .. dir .. "/")
    print("  " .. dir .. "/Makefile")
    print("  " .. dir .. "/src/app_main.c")
    print("  " .. dir .. "/src/entry.h")
    if rt_compose then
        print("  " .. dir .. "/src/app_feature_registry.c")
    end
    print("  " .. dir .. "/scripts/gen_registry.sh")
    print("  " .. dir .. "/platform/libhull_platform.a")
    if rt_compose then
        print("  " .. dir .. "/platform/libhull_feature-" .. rt_compose.name .. ".a")
    end
    if platform_lib_arm then
        print("  " .. dir .. "/platform/.aarch64/libhull_platform.a")
    end
    for _, path in ipairs(files) do
        local rel = path:sub(#opts.app_dir + 2)
        print("  " .. dir .. "/app/" .. rel)
    end
    print("")
    print("Next steps:")
    print("  cd " .. dir)
    print("  make")
    print("  ./" .. out_name)
end

-- The tool dispatcher (src/hull/tool.c) invokes the returned main() only when
-- this module is the entry command it was asked to run. A module that is
-- require()'d as a dependency (e.g. by an app during manifest extraction in
-- the tool VM) hands its main() back but is never called, so it can't run
-- against the wrong argv.
return main
