#!/usr/bin/env bash
#
# Run ONE make invocation under a stall watchdog, retrying ONLY a stall.
#
# WHY THIS EXISTS. cosmocc's cc1 blocks indefinitely at ~0 CPU on roughly 12%
# of large-translation-unit compiles on Windows (hull#462). That is a
# cosmocc-on-Windows bug, not a Hull one, and it is why every make invocation
# in windows-source-build.yml needs a watchdog rather than just a job timeout:
# a wedge otherwise burns the full cap and reports nothing useful.
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
poll=15

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
  local mk=$! last_size=0 stalled=0 size rc

  while kill -0 "$mk" 2>/dev/null; do
    sleep "$poll"
    size=$(wc -c < "$log" 2>/dev/null || echo 0)
    if [ "$size" -eq "$last_size" ]; then
      stalled=$((stalled + poll))
    else
      stalled=0
      last_size=$size
    fi
    if [ "$stalled" -ge "$stall_limit" ]; then
      echo ""
      echo "=========================================================="
      echo "STALLED [$label]: no output for ${stalled}s (limit ${stall_limit}s)"
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
