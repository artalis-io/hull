# hull_proc.sh - stop and observe a backgrounded hull from a POSIX harness.
#
# WHY THIS EXISTS. MSYS process control does not cross to a NATIVE child, and a
# Cosmopolitan APE is native on Windows. Two distinct failures follow, both
# measured on the Windows runner:
#
#   1. `kill -INT $pid` NEVER REACHES IT. MSYS emulates signals for MSYS
#      processes; SIGTERM/SIGKILL fall back to TerminateProcess but SIGINT has
#      no fallback, so it is simply dropped. A following `wait $pid` then blocks
#      forever - measured: 299 seconds of a 300-second budget in e2e_smtp, which
#      is the whole reason that suite timed out at 600s. Every OTHER command in
#      that suite was sub-second.
#
#   2. `kill -0 $pid` IS NOT A LIVENESS TEST for such a child. A backgrounded
#      native child stays unreaped until something waits on it, so `kill -0`
#      keeps succeeding long after the process is gone - it reports MSYS's
#      process table, not the process. A probe built on it claimed a process had
#      survived `taskkill /F`, which cannot happen; that impossibility is what
#      exposed the bad measurement. `tasklist` on the WINDOWS pid is the
#      authority (see hull_proc_alive).
#
# GRACEFUL STOP IS NOT DELIVERABLE FROM HERE. Keel installs a console control
# handler on Windows (vendor/keel/.../http_server_plat_win.c: CTRL_C_EVENT /
# CTRL_BREAK_EVENT / CTRL_CLOSE_EVENT -> kl_http_server_stop), so hull does shut
# down gracefully there - but only for a real console event, which MSYS cannot
# generate for another process without signalling every process on the shared
# console. Measured: kill -INT and `taskkill /PID` (no /F) both leave `wait`
# blocking; only kill -TERM and taskkill /F return, and both are TerminateProcess.
#
# So a suite must NOT substitute SIGTERM for SIGINT to "fix" a graceful-shutdown
# assertion: TerminateProcess makes "shutdown completed within N seconds"
# trivially true, turning a real assertion into a vacuous one - the exact
# broken-guard pattern this sweep has been removing. Use hull_proc_graceful to
# SKIP those assertions where the platform cannot produce them, and keep them
# unchanged everywhere else.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# hull_proc_init BIN - call once, with the binary that will be backgrounded.
hull_proc_init() {
    HULL_PROC_WIN=0
    command -v tasklist >/dev/null 2>&1 || return 0
    case "$(head -c 6 "${1:-}" 2>/dev/null || true)" in
        MZqFpD|jartsr) HULL_PROC_WIN=1 ;;
    esac
    return 0
}

# hull_proc_winpid MSYSPID -> the Windows pid, or empty.
hull_proc_winpid() { cat "/proc/${1:-}/winpid" 2>/dev/null || true; }

# hull_proc_alive PID -> 0 if the process is REALLY still running.
hull_proc_alive() {
    if [ "${HULL_PROC_WIN:-0}" = 1 ]; then
        _wp=$(hull_proc_winpid "$1")
        [ -n "$_wp" ] || return 1
        tasklist //FI "PID eq $_wp" 2>/dev/null | grep -q "[^0-9]$_wp[^0-9]" && return 0
        tasklist  /FI "PID eq $_wp" 2>/dev/null | grep -q "[^0-9]$_wp[^0-9]"
        return $?
    fi
    kill -0 "$1" 2>/dev/null
}

# hull_proc_wait PID SECS -> 0 if it exited within SECS. NEVER blocks longer:
# a watchdog SIGKILLs the child so the `wait` is always reached. SIGKILL is the
# one mechanism measured to work on both hosts.
hull_proc_wait() {
    _p=$1; _s=${2:-15}
    ( sleep "$_s"; kill -9 "$_p" 2>/dev/null ) 2>/dev/null &
    _wd=$!
    _t0=$(date +%s)
    wait "$_p" 2>/dev/null
    _el=$(( $(date +%s) - _t0 ))
    kill "$_wd" 2>/dev/null
    [ "$_el" -lt "$_s" ]
}

# hull_proc_stop PID [SECS] - make it go away, bounded, for TEARDOWN (no
# assertion rides on this). POSIX keeps the graceful-first ladder it always had;
# Windows goes straight to the only thing that works there.
hull_proc_stop() {
    _p=$1; _s=${2:-15}
    if [ "${HULL_PROC_WIN:-0}" = 1 ]; then
        kill -9 "$_p" 2>/dev/null
        hull_proc_wait "$_p" "$_s"
        return 0
    fi
    kill -INT "$_p" 2>/dev/null
    hull_proc_wait "$_p" "$_s" && return 0
    kill -TERM "$_p" 2>/dev/null; hull_proc_wait "$_p" 5 && return 0
    kill -9 "$_p" 2>/dev/null; hull_proc_wait "$_p" 5
    return 0
}

# hull_proc_graceful -> 0 if a GRACEFUL stop can actually be delivered here.
# False on Windows: see the header. Suites gate graceful-shutdown assertions on
# this and skip rather than assert something the harness cannot cause.
hull_proc_graceful() { [ "${HULL_PROC_WIN:-0}" != 1 ]; }

hull_proc_graceful_note() {
    printf '%s' "MSYS cannot deliver a console Ctrl event to a native APE, so a graceful stop is not reachable from this harness (kill -INT is dropped; SIGTERM/taskkill /F are TerminateProcess, which would make a bounded-shutdown assertion vacuous)"
}
