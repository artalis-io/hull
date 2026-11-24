#!/usr/bin/env bash
#
# Run ONE make invocation under a stall watchdog, retrying ONLY a stall.
#
# WHY THIS EXISTS. cosmocc's cc1 wedges at 0 CPU on roughly 12% of
# large-translation-unit compiles on Windows (hull#462). Measured on a Windows
# 11 machine from a full dump of a wedged cc1: it takes an access violation on
# a WRITE inside cosmo's malloc (__mspace_malloc), to the unmapped tail of a
# block dlmalloc believes it owns; GCC's crash handler (crash_signal ->
# internal_error -> xmalloc) then waits for ever on the malloc lock the faulting
# frame still holds. So it is a crash that cannot report itself, in the cosmo
# runtime linked into cosmocc 4.0.2's cc1 (built Nov 2024, before upstream's
# Dec 2024 memory-manager rework) - not a Hull bug, and not an I/O block. No GCC
# flag avoids it (raising the ggc thresholds or pinning to one core changed
# nothing), which is why every make invocation in windows-source-build.yml needs
# a watchdog rather than just a job timeout.
#
# TWO STALL TESTS. A wedged cc1 is a live process whose CPU time never moves,
# and a compiling cc1 is CPU-bound, so a cc1 whose CPU time is unchanged across
# HULL_FROZEN_POLLS polls (default 2, so 30s) is called a stall at once - a
# retry then costs ~30s instead of the 5 minutes the output-silence test takes.
# The silence test stays as the backstop for anything else that goes quiet.
#
# WHY A SCRIPT AND NOT INLINE BASH. This logic was inline in the "Build hull
# from source" step. Once a second and third make invocation existed (the unit
# tests, and the multi-arch platform build for the app-build job), copies would
# have drifted - and the retry rule here is subtle enough that a drifted copy
# is worse than none. One implementation, three callers.
#
# THE RETRY RULE, which is the whole point: a retry is applied ONLY to a stall.
# A genuine compile error fails immediately, because retrying a real error just
# hides it behind a slower red. Retrying a stall is cheap because make resumes
# from the objects already built (see the cosmo_fat_repair note in the
# workflow header for why a half-built fat pair does not poison the resume).
#
# Usage:  make-watched.sh <label> <logfile> <make-arg>...
#
# Env:
#   MAKE_BIN               required - the MSYS-native make (see workflow header)
#   HULL_STALL_LIMIT_SECS  no log output for this long = stall (default 300)
#   HULL_FROZEN_POLLS      a cc1 with unchanged CPU time this many polls in a
#                          row = stall (default 2; 0 turns the test off)
#   HULL_BUILD_ATTEMPTS    total attempts before giving up (default 3)
#
# Exit: 0 success; 1 genuine failure, or wedged on every attempt.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -uo pipefail

label=${1:?usage: make-watched.sh <label> <logfile> <make-args...>}
log=${2:?usage: make-watched.sh <label> <logfile> <make-args...>}
shift 2

: "${MAKE_BIN:?make-watched.sh: MAKE_BIN must be set to the MSYS-native make}"
stall_limit=${HULL_STALL_LIMIT_SECS:-300}
attempts=${HULL_BUILD_ATTEMPTS:-3}
frozen_polls=${HULL_FROZEN_POLLS:-2}
poll=15

# "pid cpu_ms" for every live cc1 / cc1plus. Empty when there is none.
cc1_cpu() {
  powershell.exe -NoProfile -Command \
    "Get-Process cc1,cc1plus -ErrorAction SilentlyContinue | ForEach-Object { '{0} {1}' -f \$_.Id, [int64]\$_.TotalProcessorTime.TotalMilliseconds }" \
    2>/dev/null | tr -d '\r' || true
}

# Kill anything a wedge left holding files, or the next attempt inherits a
# stuck cc1 still sitting on its output.
kill_stragglers() {
  powershell.exe -NoProfile -Command \
    "Get-Process cc1,cc1plus,cosmocc,'*cosmo-gcc' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue" \
    >/dev/null 2>&1 || true
}

# 0 = ok, 1 = genuine failure, 2 = stalled.
run_once() {
  : > "$log"
  "$MAKE_BIN" "$@" > "$log" 2>&1 &
  local mk=$! last_size=0 stalled=0 size rc why pid cpu
  declare -A prev_cpu=() frozen=()

  while kill -0 "$mk" 2>/dev/null; do
    sleep "$poll"
    size=$(wc -c < "$log" 2>/dev/null || echo 0)
    if [ "$size" -eq "$last_size" ]; then
      stalled=$((stalled + poll))
    else
      stalled=0
      last_size=$size
    fi
    why=""
    if [ "$stalled" -ge "$stall_limit" ]; then
      why="no output for ${stalled}s (limit ${stall_limit}s)"
    elif [ "$frozen_polls" -gt 0 ]; then
      declare -A now_cpu=()
      while read -r pid cpu; do
        [ -n "$pid" ] || continue
        now_cpu[$pid]=$cpu
        if [ "${prev_cpu[$pid]:-}" = "$cpu" ]; then
          frozen[$pid]=$(( ${frozen[$pid]:-0} + 1 ))
          if [ "${frozen[$pid]}" -ge "$frozen_polls" ]; then
            why="cc1 pid $pid CPU time frozen at ${cpu}ms for $((frozen[$pid] * poll))s"
          fi
        else
          frozen[$pid]=0
        fi
      done < <(cc1_cpu)
      prev_cpu=(); for pid in "${!now_cpu[@]}"; do prev_cpu[$pid]=${now_cpu[$pid]}; done
      unset now_cpu
    fi
    if [ -n "$why" ]; then
      echo ""
      echo "=========================================================="
      echo "STALLED [$label]: $why"
      echo "=========================================================="
      echo "--- last 25 lines ---"
      tail -25 "$log" || true
      echo ""
      # PowerShell is the only thing here that can see a Windows process's
      # command line and cumulative CPU. dcpu ~0 on the stuck compiler is the
      # signature of this wedge; a non-zero delta would mean something else is
      # going on and retrying is the wrong response.
      powershell.exe -NoProfile -ExecutionPolicy Bypass \
        -File "$(cygpath -w .github/scripts/dump-stuck-build.ps1)" \
        -IntervalSeconds 5 || true
      kill "$mk" 2>/dev/null || true
      kill_stragglers
      return 2
    fi
  done

  rc=0
  wait "$mk" || rc=$?
  [ "$rc" -eq 0 ] || return 1
  return 0
}

attempt=1
while : ; do
  echo "--- [$label] attempt $attempt of $attempts ---"
  run_once "$@"
  case $? in
    0) echo "[$label] ok on attempt $attempt"; cat "$log"; exit 0 ;;
    1) echo "[$label] FAIL genuine error (not a stall) - not retrying"
       tail -25 "$log"; exit 1 ;;
    2) if [ "$attempt" -ge "$attempts" ]; then
         echo "[$label] FAIL wedged on all $attempts attempts (see hull#462)"
         exit 1
       fi
       echo "[$label] wedged; retrying (make resumes from existing objects)"
       attempt=$((attempt + 1))
       ;;
  esac
done
