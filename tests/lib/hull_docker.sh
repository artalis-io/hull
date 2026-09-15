# hull_docker.sh - can this host run a LINUX container?
#
# `command -v docker` is not that question, and every suite that asked it meant
# this one. On a Windows host docker exists and is perfectly healthy - it just
# serves WINDOWS containers. Pulling a linux image there fails with
#
#     docker: no matching manifest for windows(10.0.26100)/amd64
#              in the manifest list entries
#
# and the run exits 125, so the suite sails past its "docker not available"
# skip and then FAILS on a host that was never able to run it. Measured on a
# windows-latest runner for alpine:3.20, redis:7, postgres:16 and mysql:8.
#
# `docker info` reports the daemon's OSType without pulling anything, so this
# costs one local call and cannot itself fail over the network.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

hull_docker_runs_linux() {
    command -v docker >/dev/null 2>&1 || return 1
    _os=$(docker info --format '{{.OSType}}' 2>/dev/null) || return 1
    [ "$_os" = "linux" ]
}
