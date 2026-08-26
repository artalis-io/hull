#!/usr/bin/env bash
# ci_run_logged.sh LOGFILE CMD [ARGS...]
#
# Nightly durable-diagnostics wrapper (docs/ci_architecture_design.md
# Appendix F). Runs CMD, captures its COMBINED stdout+stderr to LOGFILE (a
# deterministic path), and exits with CMD's REAL exit status - so a failing
# command still fails the job, AND the unattended nightly leaves a downloadable
# log artifact (uploaded on failure) rather than only the ephemeral Actions run
# log. Fuzz jobs keep their own crash-* artifacts and do not use this.
#
# Only the command's own output is captured - no environment dumps or secrets
# beyond what the command already prints (which is already CI-log-safe).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -o pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: ci_run_logged.sh LOGFILE CMD [ARGS...]" >&2
    exit 2
fi

log="$1"
shift
mkdir -p "$(dirname "$log")" 2>/dev/null || true

# `tee` writes LOGFILE even when CMD fails; PIPESTATUS[0] is CMD's real status
# (NOT tee's), so the wrapper's exit status equals CMD's.
"$@" 2>&1 | tee "$log"
exit "${PIPESTATUS[0]}"
