#!/bin/sh
# diag_conc.sh - does making WAL best-effort let all N processes start?
# DO NOT MERGE.
#
# Baseline on main: N=1 serves 1/1; N=2/4/8 serve 1-2. The losers opened the
# database FINE and then aborted because a tuning pragma did not apply.
#
# Controls the previous sweep lacked:
#   * a DISTINCT port range per case (it reused one range, so TIME_WAIT from an
#     earlier case could fail a later bind and look like the bug under test)
#   * a private directory per case, so the relative data.db is per-case
#   * N=8 repeated, since a lock race is probabilistic
set -u
cd "$(dirname "$0")/.." || exit 1
HULL=$(pwd)/build/hull
APP=$(pwd)/examples/hello/app.lua
BASE=$(mktemp -d)
PORT=19000

sweep() { # $1 n  $2 label
    _n=$1; _lbl=$2
    _d="$BASE/c$PORT"; mkdir -p "$_d/home"
    ( cd "$_d" || exit 1
      _pids=""; _i=0
      while [ "$_i" -lt "$_n" ]; do
          HOME="$_d/home" "$HULL" "$APP" -p $((PORT + _i)) --no-sandbox --no-migrate \
              >/dev/null 2>"$_d/err_$_i" &
          _pids="$_pids $!"
          _i=$((_i + 1))
      done
      _w=40
      while [ "$_w" -gt 0 ]; do
          _up=0; _i=0
          while [ "$_i" -lt "$_n" ]; do
              grep -q "listening on" "$_d/err_$_i" 2>/dev/null && _up=$((_up + 1))
              _i=$((_i + 1))
          done
          [ "$_up" -eq "$_n" ] && break
          sleep 0.5; _w=$((_w - 1))
      done
      if [ "$_up" -eq "$_n" ]; then
          printf '  %-14s %d/%d served   <= all started\n' "$_lbl" "$_up" "$_n"
      else
          printf '  %-14s %d/%d served\n' "$_lbl" "$_up" "$_n"
          _i=0
          while [ "$_i" -lt "$_n" ]; do
              if ! grep -q "listening on" "$_d/err_$_i" 2>/dev/null; then
                  echo "        first failure: $(head -1 "$_d/err_$_i" 2>/dev/null || echo '(silent)')"
                  break
              fi
              _i=$((_i + 1))
          done
      fi
      # Did anyone fall back to rollback mode? That is the fix working, not failing.
      grep -h "could not enable WAL" "$_d"/err_* 2>/dev/null | head -1 | sed 's/^/        note: /'
      for _p in $_pids; do kill -9 "$_p" 2>/dev/null; wait "$_p" 2>/dev/null; done )
    PORT=$((PORT + 100))
    sleep 1
}

echo "=== N-sweep with WAL best-effort (baseline on main: 1-2 served for N>=2) ==="
sweep 1 "N=1"
sweep 2 "N=2"
sweep 4 "N=4"
sweep 8 "N=8"
echo
echo "=== N=8 repeated (a lock race is probabilistic) ==="
sweep 8 "N=8 again"
sweep 8 "N=8 again"
rm -rf "$BASE"
echo "=== reached the end ==="
