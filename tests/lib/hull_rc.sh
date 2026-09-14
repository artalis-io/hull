# hull_rc.sh - recover a Cosmopolitan APE's REAL exit status on Windows.
#
# WHY THIS EXISTS. On Windows every APE reports the raw wait() status, shifted
# left by 8. MSYS2 / Git Bash / Cygwin keep only the low byte, so `$?` is 0 for
# EVERY outcome (jart/cosmopolitan#1521): `&&` does not short-circuit, `set -e`
# does not abort, and an exit-code assertion passes no matter what the program
# did. That silently turned a dozen Windows e2e suites into tests that assert
# nothing, and - worse - discarded the `|| fail "$out"` diagnostics that would
# have explained the failures they did report.
#
# The status is not LOST there, only displaced: PowerShell sees the unshifted
# value in $LASTEXITCODE. Measured with cosmocc 4.0.2 on Windows 11, exit codes
# 0/1/2/7/42/255 arrive as 0/256/512/1792/10752/65280 - exactly code<<8 - so
# shifting back recovers the code exactly. That is what these helpers do.
#
# On every other host they are a direct call and cost nothing.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# Windows iff this shell can see a PowerShell AND hull is an APE. Probing both
# avoids paying the PowerShell round-trip on a native Windows build, and avoids
# a false positive on a Linux box that happens to have pwsh installed.
hull_rc_init() {
    HULL_RC_SHIFT=0
    command -v powershell.exe >/dev/null 2>&1 || return 0
    case "$(head -c 6 "${1:-}" 2>/dev/null || true)" in
        MZqFpD|jartsr) HULL_RC_SHIFT=1 ;;
    esac
}

# hull_rc CMD [ARGS...] -> prints the REAL exit status. Output is discarded;
# use hull_run when the test needs stdout too.
hull_rc() {
    if [ "${HULL_RC_SHIFT:-0}" != 1 ]; then
        "$@" >/dev/null 2>&1
        echo $?
        return 0
    fi
    _hrc_exe=$1
    shift
    _hrc_w=$(cygpath -w "$_hrc_exe" 2>/dev/null || printf '%s' "$_hrc_exe")
    _hrc_raw=$(powershell.exe -NoProfile -Command                                       "& '$_hrc_w' $* *> \$null; Write-Output \$LASTEXITCODE" \
        2>/dev/null | tr -d '' | tail -1)
    case "$_hrc_raw" in
        ''|*[!0-9]*) echo 127; return 0 ;;
    esac
    echo $(( _hrc_raw >> 8 ))
}

# hull_run OUTFILE CMD [ARGS...] -> prints the REAL exit status and writes the
# command's combined output to OUTFILE, for the common
#   out=$(cmd); rc=$?
# shape that needs both. One invocation, so a command with side effects is not
# run twice.
#
# The output comes back through PowerShell's STDOUT with a sentinel line rather
# than through a PowerShell file redirection. `*>` in Windows PowerShell writes
# UTF-16 with a BOM, which arrives as "ÿþhello" and breaks every grep the
# callers do; its stdout is plain text that msys2 reads cleanly.
hull_run() {
    _hrn_out=$1
    shift
    if [ "${HULL_RC_SHIFT:-0}" != 1 ]; then
        "$@" > "$_hrn_out" 2>&1
        echo $?
        return 0
    fi
    _hrn_exe=$1
    shift
    _hrn_w=$(cygpath -w "$_hrn_exe" 2>/dev/null || printf '%s' "$_hrn_exe")
    powershell.exe -NoProfile -Command \
        "& '$_hrn_w' $* 2>&1 | Out-String -Stream; Write-Output \"###HULLRC=\$LASTEXITCODE\"" \
        2>/dev/null | tr -d '' > "$_hrn_out.raw"
    _hrn_raw=$(sed -n 's/^###HULLRC=//p' "$_hrn_out.raw" | tail -1)
    grep -v '^###HULLRC=' "$_hrn_out.raw" > "$_hrn_out"
    rm -f "$_hrn_out.raw"
    case "$_hrn_raw" in
        ''|*[!0-9]*) echo 127; return 0 ;;
    esac
    echo $(( _hrn_raw >> 8 ))
}
