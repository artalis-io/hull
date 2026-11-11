#!/bin/sh
# apt-fallback-mirrors.sh: give the runner's apt non-Canonical mirrors to
# fall back on.
#
# GitHub's Ubuntu runners resolve apt through mirror+file:/etc/apt/apt-mirrors.txt,
# which lists only Canonical-operated hosts (azure.archive, archive, security).
# When Canonical's DNS or archive is down every apt-get in CI fails at once.
# This appends mirrors run by institutions on Ubuntu's official mirror registry
# (Linux Foundation, Oregon State OSL, MIT, Berkeley OCF); apt tries them in
# order after the Canonical ones.
#
# Trust: apt verifies every index against the ubuntu-keyring signature on
# InRelease, and every package against the signed index's SHA-256, so a mirror
# cannot substitute a package. What a mirror COULD do is serve an older, still
# validly signed index (a freeze); that is why only named institutional
# operators are listed here, never an arbitrary mirror. Add to this list only
# operators of that standing.
#
# Usage (before apt-get update):  sh .github/scripts/apt-fallback-mirrors.sh
set -eu

list=/etc/apt/apt-mirrors.txt
if [ ! -f "$list" ]; then
    echo "apt-fallback-mirrors: $list not found; leaving apt as configured"
    exit 0
fi

case "$(dpkg --print-architecture)" in
    amd64|i386)
        mirrors="https://mirrors.edge.kernel.org/ubuntu/
https://ftp.osuosl.org/pub/ubuntu/
https://mirrors.mit.edu/ubuntu/" ;;
    *)
        mirrors="https://mirrors.mit.edu/ubuntu-ports/
https://mirrors.ocf.berkeley.edu/ubuntu-ports/" ;;
esac

SUDO=
[ "$(id -u)" -eq 0 ] || SUDO=sudo
for m in $mirrors; do
    grep -qxF "$m" "$list" || echo "$m" | $SUDO tee -a "$list" >/dev/null
done
# A failed host is retried, then the next mirror is tried for that file.
echo 'Acquire::Retries "3";' | $SUDO tee /etc/apt/apt.conf.d/80-hull-retries >/dev/null
echo "apt-fallback-mirrors: $list now:"
sed 's/^/  /' "$list"
