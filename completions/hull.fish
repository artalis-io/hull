# Hull — fish completion
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
    set -l commands keygen build verify inspect manifest test new init dev eject sign-platform migrate agent mcp check compute deploy version doctor update
    for c in $cmd[2..-1]
        if contains -- $c $commands
            return 1
        end
    end
    return 0
end

# ── Top-level commands ───────────────────────────────────────────────

complete -c hull -n __hull_no_subcommand -f -a keygen        -d 'Generate Ed25519 keypair'
complete -c hull -n __hull_no_subcommand -f -a build         -d 'Compile app into a standalone binary'
complete -c hull -n __hull_no_subcommand -f -a verify        -d 'Verify Ed25519 signatures'
complete -c hull -n __hull_no_subcommand -f -a inspect       -d 'Display declared capabilities'
complete -c hull -n __hull_no_subcommand -f -a manifest      -d 'Extract and print manifest as JSON'
complete -c hull -n __hull_no_subcommand -f -a test          -d 'Run app tests (in-process)'
complete -c hull -n __hull_no_subcommand -f -a new           -d 'Scaffold a new hull project'
complete -c hull -n __hull_no_subcommand -f -a init          -d 'Initialize hull in current directory'
complete -c hull -n __hull_no_subcommand -f -a dev           -d 'Development server with hot reload'
complete -c hull -n __hull_no_subcommand -f -a eject         -d 'Export to standalone Makefile project'
complete -c hull -n __hull_no_subcommand -f -a sign-platform -d 'Sign platform library'
complete -c hull -n __hull_no_subcommand -f -a migrate       -d 'Run SQL migrations'
complete -c hull -n __hull_no_subcommand -f -a agent         -d 'Agent introspection (JSON)'
complete -c hull -n __hull_no_subcommand -f -a mcp           -d 'Model Context Protocol server'
complete -c hull -n __hull_no_subcommand -f -a check         -d 'Pre-flight checks on a project'
complete -c hull -n __hull_no_subcommand -f -a compute       -d 'WASM compute plugin tools'
complete -c hull -n __hull_no_subcommand -f -a deploy        -d 'Generate deployment configs'
complete -c hull -n __hull_no_subcommand -f -a version       -d 'Print hull version'
complete -c hull -n __hull_no_subcommand -f -a doctor        -d 'Check hull build readiness'
complete -c hull -n __hull_no_subcommand -f -a update        -d 'Self-update from GitHub releases'

complete -c hull -n __hull_no_subcommand -s v -l version     -d 'Print hull version'

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

# ── test / check / inspect / verify / manifest / compute / eject ─────

for cmd in test check inspect verify manifest compute eject
    complete -c hull -n "__hull_subcommand $cmd" -l json -d 'Machine-readable JSON output'
    complete -c hull -n "__hull_subcommand $cmd" -l developer-key -r -d 'Developer public key'
end

# ── new / init ───────────────────────────────────────────────────────

for cmd in new init
    complete -c hull -n "__hull_subcommand $cmd" -l runtime -r -d 'Runtime' -xa 'lua js'
end

# ── agent ────────────────────────────────────────────────────────────

complete -c hull -n '__hull_subcommand agent' -f -a routes  -d 'List routes + middleware'
complete -c hull -n '__hull_subcommand agent' -f -a db      -d 'DB introspection (schema/query)'
complete -c hull -n '__hull_subcommand agent' -f -a request -d 'HTTP request to dev server'
complete -c hull -n '__hull_subcommand agent' -f -a status  -d 'Check dev server status'
complete -c hull -n '__hull_subcommand agent' -f -a errors  -d 'Structured errors from last reload'
complete -c hull -n '__hull_subcommand agent' -f -a test    -d 'Run tests with JSON output'
complete -c hull -n '__hull_subcommand agent' -f -a context -d 'Task-relevant documentation'
complete -c hull -n '__hull_subcommand agent' -f -a migrate -d 'Migration status'
complete -c hull -n '__hull_subcommand agent' -f -a deploy  -d 'Deployment readiness analysis'

# agent db subcommands
complete -c hull -n '__hull_agent_sub db' -f -a schema -d 'Introspect DB schema'
complete -c hull -n '__hull_agent_sub db' -f -a query  -d 'Run read-only SQL query'

# agent request HTTP methods
complete -c hull -n '__hull_agent_sub request' -f -a 'GET POST PUT PATCH DELETE HEAD OPTIONS'

# agent context flags
complete -c hull -n '__hull_agent_sub context' -l task  -r -d 'Task' \
    -xa 'auth db routing middleware testing build deploy validation templates i18n search webhooks'
complete -c hull -n '__hull_agent_sub context' -l level -r -d 'Detail level' -xa 'brief detailed'

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
