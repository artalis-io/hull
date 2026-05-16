# Hull — bash completion
#
# Install:
#   # System-wide:
#   sudo cp hull.bash /etc/bash_completion.d/hull
#   # User-local:
#   mkdir -p ~/.local/share/bash-completion/completions
#   cp hull.bash ~/.local/share/bash-completion/completions/hull
#
# SPDX-License-Identifier: AGPL-3.0-or-later

_hull() {
    local cur prev words cword
    _init_completion 2>/dev/null || {
        # Fallback for systems without bash-completion installed
        COMPREPLY=()
        cur="${COMP_WORDS[COMP_CWORD]}"
        prev="${COMP_WORDS[COMP_CWORD-1]}"
        words=("${COMP_WORDS[@]}")
        cword=$COMP_CWORD
    }

    local commands="keygen build verify inspect manifest test new init dev eject sign-platform migrate agent mcp check compute deploy version doctor update"
    # Keep in sync with src/hull/commands/agent.c hl_cmd_agent() dispatch table.
    local agent_subs="routes db request status errors test context migrate deploy manifest vfs compute compute-call gpu capabilities validate logs endpoint middleware eval perf template schema-diff sql"
    local agent_db_subs="schema query"
    local migrate_subs="status new"
    local deploy_targets="dockerfile systemd fly"
    local runtimes="lua js"
    local compilers="tcc system cc gcc clang cosmocc"

    # Global flags (always available)
    local global_flags="--version -v"

    # ── Top-level command ───────────────────────────────────────────
    if [ "$cword" -eq 1 ]; then
        if [[ "$cur" == -* ]]; then
            COMPREPLY=($(compgen -W "$global_flags" -- "$cur"))
        else
            COMPREPLY=($(compgen -W "$commands" -- "$cur"))
        fi
        return
    fi

    local subcommand="${words[1]}"

    # ── Per-subcommand handling ─────────────────────────────────────
    case "$subcommand" in
        build)
            case "$prev" in
                -o|--output|--compiler|--target|--sign|--runtime|--developer-key)
                    case "$prev" in
                        --compiler) COMPREPLY=($(compgen -W "$compilers" -- "$cur")) ;;
                        --runtime)       COMPREPLY=($(compgen -W "$runtimes" -- "$cur")) ;;
                        --target)        COMPREPLY=($(compgen -W "x86_64 aarch64" -- "$cur")) ;;
                        -o|--output|--sign|--developer-key)
                                         COMPREPLY=($(compgen -f -- "$cur")) ;;
                    esac
                    return ;;
            esac
            if [[ "$cur" == --compiler=* ]]; then
                COMPREPLY=($(compgen -W "$compilers" -- "${cur#--compiler=}"))
                return
            fi
            if [[ "$cur" == -* ]]; then
                COMPREPLY=($(compgen -W "-o --output --compiler --sign --runtime --target --no-aot --developer-key" -- "$cur"))
            else
                COMPREPLY=($(compgen -d -- "$cur"))
            fi
            ;;

        dev)
            case "$prev" in
                -p|--port|-d|--database|--ca-bundle)
                    case "$prev" in
                        -d|--database|--ca-bundle)
                            COMPREPLY=($(compgen -f -- "$cur")) ;;
                    esac
                    return ;;
            esac
            if [[ "$cur" == --ca-bundle=* ]]; then
                COMPREPLY=($(compgen -f -- "${cur#--ca-bundle=}"))
                return
            fi
            if [[ "$cur" == -* ]]; then
                COMPREPLY=($(compgen -W "-p --port -d --database --agent --no-migrate --no-sandbox --audit --ca-bundle --no-ca-bundle" -- "$cur"))
            else
                COMPREPLY=($(compgen -f -- "$cur"))
            fi
            ;;

        test|check|inspect|verify|manifest|eject)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=($(compgen -W "--json --developer-key" -- "$cur"))
            else
                COMPREPLY=($(compgen -d -- "$cur"))
            fi
            ;;

        compute)
            # `hull compute <subcmd>` — complete the subcommand name.
            if [[ ${COMP_CWORD} -eq 2 ]]; then
                COMPREPLY=($(compgen -W "new build test check refresh-header" -- "$cur"))
                return
            fi
            # `hull compute new <name> [--lang c]`
            local subcmd="${COMP_WORDS[2]}"
            case "$subcmd" in
                new)
                    case "$prev" in
                        --lang) COMPREPLY=($(compgen -W "c" -- "$cur")); return ;;
                    esac
                    if [[ "$cur" == -* ]]; then
                        COMPREPLY=($(compgen -W "--lang" -- "$cur"))
                    fi
                    ;;
                build|test|check|refresh-header)
                    # Complete with existing module names from compute/<name>/<name>.c.
                    if [[ -d compute ]]; then
                        local mods=$(compgen -G "compute/*/*.c" 2>/dev/null | \
                                     sed 's|compute/||; s|/.*\.c$||' | sort -u)
                        COMPREPLY=($(compgen -W "$mods" -- "$cur"))
                    fi
                    ;;
            esac
            ;;

        new|init)
            case "$prev" in
                --runtime) COMPREPLY=($(compgen -W "$runtimes" -- "$cur")); return ;;
            esac
            if [[ "$cur" == --runtime=* ]]; then
                COMPREPLY=($(compgen -W "$runtimes" -- "${cur#--runtime=}"))
                return
            fi
            if [[ "$cur" == -* ]]; then
                COMPREPLY=($(compgen -W "--runtime" -- "$cur"))
            else
                COMPREPLY=($(compgen -d -- "$cur"))
            fi
            ;;

        agent)
            if [ "$cword" -eq 2 ]; then
                COMPREPLY=($(compgen -W "$agent_subs" -- "$cur"))
                return
            fi
            local agent_sub="${words[2]}"
            case "$agent_sub" in
                db)
                    if [ "$cword" -eq 3 ]; then
                        COMPREPLY=($(compgen -W "$agent_db_subs" -- "$cur"))
                    fi
                    ;;
                request)
                    if [ "$cword" -eq 3 ]; then
                        COMPREPLY=($(compgen -W "GET POST PUT PATCH DELETE HEAD OPTIONS" -- "$cur"))
                    fi
                    ;;
                context)
                    if [[ "$cur" == --task=* ]]; then
                        COMPREPLY=($(compgen -W "auth db routing middleware testing build deploy validation templates i18n search webhooks" -- "${cur#--task=}"))
                    elif [[ "$cur" == --level=* ]]; then
                        COMPREPLY=($(compgen -W "brief detailed" -- "${cur#--level=}"))
                    elif [[ "$cur" == -* ]]; then
                        COMPREPLY=($(compgen -W "--task= --level=" -- "$cur"))
                    fi
                    ;;
                *)
                    if [[ "$cur" == -* ]]; then
                        COMPREPLY=($(compgen -W "--json -p" -- "$cur"))
                    else
                        COMPREPLY=($(compgen -d -- "$cur"))
                    fi
                    ;;
            esac
            ;;

        migrate)
            if [ "$cword" -eq 2 ] && [[ "$cur" != -* ]]; then
                COMPREPLY=($(compgen -W "$migrate_subs" -d -- "$cur"))
            elif [[ "$cur" == -* ]]; then
                COMPREPLY=($(compgen -W "-d --database --json" -- "$cur"))
            else
                COMPREPLY=($(compgen -d -- "$cur"))
            fi
            ;;

        deploy)
            if [ "$cword" -eq 2 ]; then
                COMPREPLY=($(compgen -W "$deploy_targets" -- "$cur"))
            else
                COMPREPLY=($(compgen -d -- "$cur"))
            fi
            ;;

        doctor|version)
            COMPREPLY=($(compgen -W "--json" -- "$cur"))
            ;;

        update)
            COMPREPLY=($(compgen -W "--check --force --repo= --help" -- "$cur"))
            ;;

        keygen|sign-platform)
            COMPREPLY=($(compgen -f -- "$cur"))
            ;;

        mcp)
            COMPREPLY=($(compgen -d -- "$cur"))
            ;;
    esac
}

complete -F _hull hull
