# hull_rc.sh - recover a Cosmopolitan APE's REAL exit status on Windows.
#
# WHY THIS EXISTS. On Windows every APE reports the raw wait() status, shifted
# left by 8. MSYS2 / Git Bash / Cygwin keep only the low byte, so `$?` is 0 for
# EVERY outcome (jart/cosmopolitan#1521): `&&` does not short-circuit, `set -e`
# does not abort, and an exit-code assertion passes no matter what happened.
# That turned a dozen Windows e2e suites into tests that assert nothing, and it
# also swallowed the `|| fail "$out"` diagnostics that would have explained the
# failures they did report - so the symptoms that surfaced pointed elsewhere.
#
# The status is not LOST there, only displaced. Measured with cosmocc 4.0.2 on
# Windows 11: exit codes 0/1/2/7/42/255 come back as 0/256/512/1792/10752/65280,
# exactly code<<8. Shifting back recovers the code exactly.
#
# HOW. Start-Process with real file handles, NOT `& 'exe'` in a PowerShell
# pipeline. The pipeline route corrupts data in both directions: Get-Content
# prepends a BOM to stdin (an "empty stdin" test then sees three bytes), and
# `*>` redirection writes stdout as UTF-16-with-BOM, which arrives as
# "ÿþhello" and breaks every grep a caller does. Start-Process passes
# byte-clean handles and reports .ExitCode directly.
#
# On every other host these are a direct call and cost nothing.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# Take the Windows path only when a PowerShell exists AND the binary really is
# an APE. Probing both means a Linux host with pwsh installed is unaffected, and
# a native Windows build pays nothing.
hull_rc_init() {
    HULL_RC_SHIFT=0
    command -v powershell.exe >/dev/null 2>&1 || return 0
    case "$(head -c 6 "${1:-}" 2>/dev/null || true)" in
        MZqFpD|jartsr) HULL_RC_SHIFT=1 ;;
    esac
}

# Internal: run via Start-Process, print the raw (shifted) status.
#   $1 out file, $2 err file, $3 stdin file or "", rest = cmd + args
hull_rc__pwsh() {
    _o=$1; _e=$2; _i=$3; shift 3
    _exe=$1; shift
    _ps="Start-Process -FilePath '$(cygpath -w "$_exe")'"
    if [ $# -gt 0 ]; then
        _al=""
        for _a in "$@"; do
            # Start-Process JOINS -ArgumentList into one command line, so an
            # argument containing spaces is re-split by the callee unless it
            # carries its own quotes. Wrap each in double quotes inside the
            # PowerShell single-quoted string; double any embedded single quote
            # so the PowerShell literal stays intact.
            _a=$(printf '%s' "$_a" | sed "s/'/''/g")
            _al="$_al,'\"$_a\"'"
        done
        _ps="$_ps -ArgumentList $(printf '%s' "$_al" | cut -c2-)"
    fi
    _ps="$_ps -RedirectStandardOutput '$(cygpath -w "$_o")'"
    _ps="$_ps -RedirectStandardError '$(cygpath -w "$_e")'"
    [ -n "$_i" ] && _ps="$_ps -RedirectStandardInput '$(cygpath -w "$_i")'"
    _ps="$_ps -NoNewWindow -Wait -PassThru"
    powershell.exe -NoProfile -Command \
        "\$p = $_ps; Write-Output \$p.ExitCode" \
        2>/dev/null | tr -d "$(printf '\\r')" | tail -1
}

# hull_rc CMD [ARGS...] -> the REAL exit status. Output discarded.
# Set HULL_RC_STDIN=FILE to feed the process stdin from a file.
hull_rc() {
    if [ "${HULL_RC_SHIFT:-0}" != 1 ]; then
        # `|| _r=$?` is not stylistic. Callers run under `set -e`, which a shell
        # function INHERITS, so a bare `"$@"` returning non-zero aborts the whole
        # script before `echo $?` runs - the script then exits with the command's
        # own code and make reports e.g. "Error 7", the app's exit status, as a
        # build failure. The code this replaced was written `rc=0; cmd || rc=$?`
        # for exactly this reason.
        _r=0
        if [ -n "${HULL_RC_STDIN:-}" ]; then
            "$@" < "$HULL_RC_STDIN" >/dev/null 2>&1 || _r=$?
        else
            "$@" >/dev/null 2>&1 || _r=$?
        fi
        echo "$_r"
        return 0
    fi
    _t=$(mktemp -d)
    _raw=$(hull_rc__pwsh "$_t/o" "$_t/e" "${HULL_RC_STDIN:-}" "$@")
    rm -rf "$_t"
    case "$_raw" in ''|*[!0-9]*) echo 127; return 0 ;; esac
    echo $(( _raw >> 8 ))
}

# hull_run OUTFILE CMD [ARGS...] -> the REAL exit status, with the command's
# combined output in OUTFILE. One invocation, so a command with side effects is
# not run twice.
hull_run() {
    _out=$1; shift
    if [ "${HULL_RC_SHIFT:-0}" != 1 ]; then
        # See hull_rc: guarded so an inherited `set -e` cannot abort the caller.
        _r=0
        if [ -n "${HULL_RC_STDIN:-}" ]; then
            "$@" < "$HULL_RC_STDIN" > "$_out" 2>&1 || _r=$?
        else
            "$@" > "$_out" 2>&1 || _r=$?
        fi
        echo "$_r"
        return 0
    fi
    _t=$(mktemp -d)
    _raw=$(hull_rc__pwsh "$_t/o" "$_t/e" "${HULL_RC_STDIN:-}" "$@")
    cat "$_t/o" "$_t/e" 2>/dev/null | tr -d "$(printf '\\r')" > "$_out"
    rm -rf "$_t"
    case "$_raw" in ''|*[!0-9]*) echo 127; return 0 ;; esac
    echo $(( _raw >> 8 ))
}
