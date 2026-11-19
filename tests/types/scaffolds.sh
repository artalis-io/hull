#!/bin/sh
# scaffolds.sh - type-check every `hull new --runtime ts` layout (and `hull
# init --runtime ts`: server, --cli, --profile htmx) with the
# pinned TypeScript: the app (`tsc -p <app>`) and its tests (`-p <app>/tests`),
# against the declarations and tsconfig.json the scaffold installs.
#
# Usage: sh tests/types/scaffolds.sh HULL TSC WORKDIR
set -eu
HULL=$1
TSC=$2
WORK=$3
case "$HULL" in /*) ;; *) HULL="$PWD/$HULL" ;; esac
case "$TSC" in /*) ;; *) TSC="$PWD/$TSC" ;; esac
rm -rf "$WORK"
mkdir -p "$WORK"
for spec in "flat:" "cliflat:--cli" "rest:--type rest" "cli:--type cli"; do
    name=${spec%%:*}; args=${spec#*:}
    # shellcheck disable=SC2086
    (cd "$WORK" && "$HULL" new "$name" --runtime ts $args > /dev/null)
    "$TSC" --noEmit -p "$WORK/$name"
    "$TSC" --noEmit -p "$WORK/$name/tests"
    echo "scaffolds: $name type-checks"
done
for spec in "isrv:" "icli:--cli" "ihx:--profile htmx"; do
    name=${spec%%:*}; args=${spec#*:}
    # shellcheck disable=SC2086
    (cd "$WORK" && "$HULL" init "$name" --runtime ts $args > /dev/null)
    "$TSC" --noEmit -p "$WORK/$name"
    "$TSC" --noEmit -p "$WORK/$name/tests"
    echo "scaffolds: hull init $name type-checks"
done
