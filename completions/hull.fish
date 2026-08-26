# Hull - fish completion
#
# Install:
#   mkdir -p ~/.config/fish/completions
#   cp hull.fish ~/.config/fish/completions/
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# ── Helpers ──────────────────────────────────────────────────────────

function __hull_subcommand
    set -l cmd (commandline -opc)
    if test (count $cmd) -lt 2
        return 1
    end
    test $cmd[2] = $argv[1]
end

function __hull_agent_sub
    set -l cmd (commandline -opc)
    if test (count $cmd) -lt 3
        return 1
    end
    test $cmd[2] = "agent" -a $cmd[3] = $argv[1]
end

function __hull_no_subcommand
    set -l cmd (commandline -opc)
    set -l commands keygen build verify verify-self verify-release sign-release sbom inspect manifest test new init dev eject sign-platform migrate modules agent mcp check compute deploy version doctor update tools help
    for c in $cmd[2..-1]
        if contains -- $c $commands
            return 1
        end
    end
    return 0
end

# ── Top-level commands ───────────────────────────────────────────────

complete -c hull -n __hull_no_subcommand -f -a keygen         -d 'Generate Ed25519 keypair'
complete -c hull -n __hull_no_subcommand -f -a build          -d 'Compile app into a standalone binary'
complete -c hull -n __hull_no_subcommand -f -a verify         -d 'Verify Ed25519 signatures'
complete -c hull -n __hull_no_subcommand -f -a verify-self    -d 'Verify hull binary against signed release manifest'
complete -c hull -n __hull_no_subcommand -f -a verify-release -d 'Verify a release manifest signature (offline)'
complete -c hull -n __hull_no_subcommand -f -a sign-release   -d 'Sign a release manifest (CI use)'
complete -c hull -n __hull_no_subcommand -f -a sbom           -d 'Print Software Bill of Materials (human/json/cyclonedx/spdx)'
complete -c hull -n __hull_no_subcommand -f -a inspect        -d 'Display declared capabilities'
complete -c hull -n __hull_no_subcommand -f -a manifest       -d 'Extract and print manifest as JSON'
complete -c hull -n __hull_no_subcommand -f -a test           -d 'Run app tests (in-process)'
complete -c hull -n __hull_no_subcommand -f -a new            -d 'Scaffold a new hull project'
complete -c hull -n __hull_no_subcommand -f -a init           -d 'Initialize hull in current directory'
complete -c hull -n __hull_no_subcommand -f -a dev            -d 'Development server with hot reload'
complete -c hull -n __hull_no_subcommand -f -a eject          -d 'Export to standalone Makefile project'
complete -c hull -n __hull_no_subcommand -f -a sign-platform  -d 'Sign platform library'
complete -c hull -n __hull_no_subcommand -f -a migrate        -d 'Run SQL migrations'
complete -c hull -n __hull_no_subcommand -f -a modules        -d 'Inspect first-party module registry + app declarations'
complete -c hull -n __hull_no_subcommand -f -a agent          -d 'Agent introspection (JSON)'
complete -c hull -n __hull_no_subcommand -f -a mcp            -d 'Model Context Protocol server'
complete -c hull -n __hull_no_subcommand -f -a check          -d 'Pre-flight checks on a project'
complete -c hull -n __hull_no_subcommand -f -a compute        -d 'WASM compute plugin tools'
complete -c hull -n __hull_no_subcommand -f -a deploy         -d 'Generate deployment configs'
complete -c hull -n __hull_no_subcommand -f -a version        -d 'Print hull version'
complete -c hull -n __hull_no_subcommand -f -a doctor         -d 'Check hull build readiness'
complete -c hull -n __hull_no_subcommand -f -a update         -d 'Self-update from GitHub releases'
complete -c hull -n __hull_no_subcommand -f -a tools          -d 'Install / list / uninstall side-loaded tools'
complete -c hull -n __hull_no_subcommand -f -a help           -d 'Show top-level usage'

complete -c hull -n __hull_no_subcommand -s v -l version     -d 'Print hull version'
complete -c hull -n __hull_no_subcommand -s h -l help        -d 'Show top-level usage'

# ── tools ────────────────────────────────────────────────────────────
# Keep tool names in sync with REGISTRY in src/hull/tools_install.c.

function __hull_tools_no_verb
    set -l cmd (commandline -opc)
    set -l verbs list install uninstall remove
    set -l seen 0
    for c in $cmd[2..-1]
        if test "$c" = "tools"
            set seen 1
        else if test $seen -eq 1
            if contains -- $c $verbs
                return 1
            end
        end
    end
    test $seen -eq 1
end

function __hull_tools_verb_is
    set -l cmd (commandline -opc)
    set -l want $argv[1]
    set -l seen_tools 0
    for c in $cmd[2..-1]
        if test "$c" = "tools"
            set seen_tools 1
        else if test $seen_tools -eq 1; and test "$c" = "$want"
            return 0
        end
    end
    return 1
end

complete -c hull -n __hull_tools_no_verb -f -a list      -d 'Print registry + install state'
complete -c hull -n __hull_tools_no_verb -f -a install   -d 'Download, verify, install a tool'
complete -c hull -n __hull_tools_no_verb -f -a uninstall -d 'Remove an installed tool'

complete -c hull -n '__hull_tools_verb_is list'      -l json -d 'Machine-readable JSON output'
complete -c hull -n '__hull_tools_verb_is install'   -l all  -d 'Install every published tool'
complete -c hull -n '__hull_tools_verb_is install'   -f -a wamrc -d 'WAMR AOT compiler'
complete -c hull -n '__hull_tools_verb_is uninstall' -f -a wamrc -d 'WAMR AOT compiler'

# ── build ────────────────────────────────────────────────────────────

complete -c hull -n '__hull_subcommand build' -s o -l output  -r -d 'Output binary path'
complete -c hull -n '__hull_subcommand build' -l compiler     -r -d 'C compiler backend' \
    -xa 'tcc system cc gcc clang cosmocc'
complete -c hull -n '__hull_subcommand build' -l runtime      -r -d 'Runtime to use' \
    -xa 'lua js'
complete -c hull -n '__hull_subcommand build' -l target       -r -d 'Cross-compile target' \
    -xa 'x86_64 aarch64'
complete -c hull -n '__hull_subcommand build' -l sign         -r -d 'Signing key file'
complete -c hull -n '__hull_subcommand build' -l developer-key -r -d 'Developer public key'
complete -c hull -n '__hull_subcommand build' -l no-aot       -d 'Skip AOT compilation'

# ── dev ──────────────────────────────────────────────────────────────

complete -c hull -n '__hull_subcommand dev' -s p -l port      -r -d 'Listen port'
complete -c hull -n '__hull_subcommand dev' -s d -l database  -r -d 'Database file'
complete -c hull -n '__hull_subcommand dev' -l agent          -d 'Enable agent sidecar files'
complete -c hull -n '__hull_subcommand dev' -l no-migrate     -d 'Skip auto-migrate on startup'
complete -c hull -n '__hull_subcommand dev' -l no-sandbox     -d 'Disable kernel sandbox'
complete -c hull -n '__hull_subcommand dev' -l audit          -d 'Enable capability audit logging'
complete -c hull -n '__hull_subcommand dev' -l ca-bundle      -r -d 'Custom CA bundle path'
complete -c hull -n '__hull_subcommand dev' -l no-ca-bundle -d 'Skip TLS verification (dev only)'

# ── test / check / inspect / verify / manifest / eject ──────────────

for cmd in test check inspect verify manifest eject
    complete -c hull -n "__hull_subcommand $cmd" -l json -d 'Machine-readable JSON output'
    complete -c hull -n "__hull_subcommand $cmd" -l developer-key -r -d 'Developer public key'
end

# ── compute (sub-subcommands) ─────────────────────────────────────────
#
# `hull compute new <name> [--lang c]`
# `hull compute build [name]`
# `hull compute test  <name>`
# `hull compute check <name>`

function __hull_compute_no_subsub
    set -l tokens (commandline -opc)
    test (count $tokens) -eq 2 -a "$tokens[2]" = compute
end

function __hull_compute_at_subsub
    set -l tokens (commandline -opc)
    test (count $tokens) -ge 3 -a "$tokens[2]" = compute
end

function __hull_compute_subsub_is
    set -l tokens (commandline -opc)
    test (count $tokens) -ge 3 -a "$tokens[2]" = compute -a "$tokens[3]" = $argv[1]
end

function __hull_compute_modules
    if test -d compute
        for c in compute/*/*.c
            set -l rel (string replace -r '^compute/' '' -- $c)
            set -l name (string split -- '/' $rel)[1]
            echo $name
        end | sort -u
    end
end

complete -c hull -n __hull_compute_no_subsub -f -a new            -d 'Scaffold a new compute module'
complete -c hull -n __hull_compute_no_subsub -f -a build          -d 'Compile module(s) source -> .wasm'
complete -c hull -n __hull_compute_no_subsub -f -a test           -d 'Run fixtures against a module'
complete -c hull -n __hull_compute_no_subsub -f -a check          -d 'Validate a .wasm module loads'
complete -c hull -n __hull_compute_no_subsub -f -a refresh-header -d 'Update per-module hull_compute.h'

complete -c hull -n '__hull_compute_subsub_is new' -l lang -r -d 'Source language' -xa 'c'

for subsub in build test check refresh-header
    complete -c hull -n "__hull_compute_subsub_is $subsub" -f -a '(__hull_compute_modules)'
end

# ── new / init ───────────────────────────────────────────────────────

for cmd in new init
    complete -c hull -n "__hull_subcommand $cmd" -l runtime -r -d 'Runtime' -xa 'lua js'
end

# ── agent ────────────────────────────────────────────────────────────
# Keep in sync with src/hull/commands/agent.c hl_cmd_agent() dispatch table.

complete -c hull -n '__hull_subcommand agent' -f -a routes       -d 'List routes + middleware'
complete -c hull -n '__hull_subcommand agent' -f -a db           -d 'DB introspection (schema/query)'
complete -c hull -n '__hull_subcommand agent' -f -a request      -d 'HTTP request to dev server'
complete -c hull -n '__hull_subcommand agent' -f -a status       -d 'Check dev server status'
complete -c hull -n '__hull_subcommand agent' -f -a errors       -d 'Structured errors from last reload'
complete -c hull -n '__hull_subcommand agent' -f -a test         -d 'Run tests with JSON output'
complete -c hull -n '__hull_subcommand agent' -f -a context      -d 'Task-relevant documentation'
complete -c hull -n '__hull_subcommand agent' -f -a migrate      -d 'Migration status'
complete -c hull -n '__hull_subcommand agent' -f -a deploy       -d 'Deployment readiness analysis'
complete -c hull -n '__hull_subcommand agent' -f -a manifest     -d 'Manifest as structured JSON'
complete -c hull -n '__hull_subcommand agent' -f -a vfs          -d 'List embedded VFS entries'
complete -c hull -n '__hull_subcommand agent' -f -a compute      -d 'WASM compute module inventory'
complete -c hull -n '__hull_subcommand agent' -f -a compute-call -d 'Invoke a WASM compute module'
complete -c hull -n '__hull_subcommand agent' -f -a gpu          -d 'GPU device + shader inventory'
complete -c hull -n '__hull_subcommand agent' -f -a capabilities -d 'Resolved capability surface'
complete -c hull -n '__hull_subcommand agent' -f -a validate     -d 'Validate a file against a schema'
complete -c hull -n '__hull_subcommand agent' -f -a logs         -d 'Tail recent dev-server logs'
complete -c hull -n '__hull_subcommand agent' -f -a endpoint     -d 'Describe a single route in detail'
complete -c hull -n '__hull_subcommand agent' -f -a middleware   -d 'Enumerate registered middleware'
complete -c hull -n '__hull_subcommand agent' -f -a eval         -d 'Evaluate Lua/JS in the app sandbox'
complete -c hull -n '__hull_subcommand agent' -f -a perf         -d 'Per-request performance snapshot'
complete -c hull -n '__hull_subcommand agent' -f -a template     -d 'Render a template with sample data'
complete -c hull -n '__hull_subcommand agent' -f -a schema-diff  -d 'Diff applied schema vs migrations'
complete -c hull -n '__hull_subcommand agent' -f -a sql          -d 'Read-only SQL query against app DB'
complete -c hull -n '__hull_subcommand agent' -f -a tools        -d 'Side-loaded tool registry + install state'
complete -c hull -n '__hull_subcommand agent' -f -a overview     -d 'Single-shot project summary'

# agent db subcommands
complete -c hull -n '__hull_agent_sub db' -f -a schema -d 'Introspect DB schema'
complete -c hull -n '__hull_agent_sub db' -f -a query  -d 'Run read-only SQL query'

# agent request HTTP methods
complete -c hull -n '__hull_agent_sub request' -f -a 'GET POST PUT PATCH DELETE HEAD OPTIONS'

# agent context flags. Task list mirrors stdlib/context/*.md.
complete -c hull -n '__hull_agent_sub context' -l task  -r -d 'Task' \
    -xa 'auth build compute db deploy gpu i18n middleware orientation quickstart-cli quickstart-tui quickstart-web routing search templates testing tools validation webhooks'
complete -c hull -n '__hull_agent_sub context' -l level -r -d 'Detail level' \
    -xa 'minimal compact full'
complete -c hull -n '__hull_agent_sub context' -l list  -d 'Enumerate context tasks'
complete -c hull -n '__hull_agent_sub context' -l interactive -d 'Interactive TUI picker'
complete -c hull -n '__hull_agent_sub context' -l tui   -d 'Interactive TUI picker'

# agent: shared flags
complete -c hull -n '__hull_subcommand agent' -l json -d 'Machine-readable JSON output'
complete -c hull -n '__hull_subcommand agent' -s p -l port -r -d 'Dev server port'

# ── migrate ──────────────────────────────────────────────────────────

complete -c hull -n '__hull_subcommand migrate' -f -a status -d 'Show migration status'
complete -c hull -n '__hull_subcommand migrate' -f -a new    -d 'Create a new migration file'
complete -c hull -n '__hull_subcommand migrate' -s d -l database -r -d 'Database file'
complete -c hull -n '__hull_subcommand migrate' -l json -d 'Machine-readable JSON output'

# ── deploy ───────────────────────────────────────────────────────────

complete -c hull -n '__hull_subcommand deploy' -f -a dockerfile -d 'Generate Dockerfile + .dockerignore'
complete -c hull -n '__hull_subcommand deploy' -f -a systemd    -d 'Generate systemd service unit'
complete -c hull -n '__hull_subcommand deploy' -f -a fly        -d 'Generate fly.toml'

# ── doctor / version ─────────────────────────────────────────────────

complete -c hull -n '__hull_subcommand doctor'  -l json -d 'Machine-readable JSON output'
complete -c hull -n '__hull_subcommand version' -l json -d 'Machine-readable JSON output'

# ── update ───────────────────────────────────────────────────────────

complete -c hull -n '__hull_subcommand update' -l check -d 'Check for an update without installing'
complete -c hull -n '__hull_subcommand update' -l force -d 'Reinstall even if version matches'
complete -c hull -n '__hull_subcommand update' -l repo  -r -d 'GitHub repo'

# ── sbom ─────────────────────────────────────────────────────────────

complete -c hull -n '__hull_subcommand sbom' -l format -r -d 'Output format' \
    -xa 'human json cyclonedx spdx'
complete -c hull -n '__hull_subcommand sbom' -l json   -d 'Shortcut for --format=json'

# ── verify-self / verify-release / sign-release ──────────────────────

complete -c hull -n '__hull_subcommand verify-self' -l manifest  -r -d 'Manifest file'
complete -c hull -n '__hull_subcommand verify-self' -l signature -r -d 'Signature file'
complete -c hull -n '__hull_subcommand verify-self' -l asset     -r -d 'Asset name in the manifest'
complete -c hull -n '__hull_subcommand verify-self' -l pubkey    -r -d 'Override embedded release public key'

complete -c hull -n '__hull_subcommand verify-release' -l pubkey -r -d 'Override embedded release public key'

complete -c hull -n '__hull_subcommand sign-release' -l key    -r -d 'Ed25519 secret key file'
complete -c hull -n '__hull_subcommand sign-release' -l output -r -d 'Output signature path'

# ── modules ──────────────────────────────────────────────────────────

function __hull_modules_no_verb
    set -l cmd (commandline -opc)
    set -l verbs list available explain analyze
    set -l seen 0
    for c in $cmd[2..-1]
        if test "$c" = "modules"
            set seen 1
        else if test $seen -eq 1
            if contains -- $c $verbs
                return 1
            end
        end
    end
    test $seen -eq 1
end

function __hull_modules_verb_is
    set -l cmd (commandline -opc)
    set -l want $argv[1]
    set -l seen 0
    for c in $cmd[2..-1]
        if test "$c" = "modules"
            set seen 1
        else if test $seen -eq 1; and test "$c" = "$want"
            return 0
        end
    end
    return 1
end

complete -c hull -n __hull_modules_no_verb -f -a list      -d 'Print modules an app declares'
complete -c hull -n __hull_modules_no_verb -f -a available -d 'Print the first-party module registry'
complete -c hull -n __hull_modules_no_verb -f -a explain   -d 'Print the spec for one module'
complete -c hull -n __hull_modules_no_verb -f -a analyze   -d 'Walk source for undeclared imports'

complete -c hull -n '__hull_modules_verb_is list'      -l json -d 'Machine-readable JSON output'
complete -c hull -n '__hull_modules_verb_is available' -l json -d 'Machine-readable JSON output'
complete -c hull -n '__hull_modules_verb_is available' -l tui  -d 'Interactive picker'
complete -c hull -n '__hull_modules_verb_is analyze'   -l json -d 'Machine-readable JSON output'
