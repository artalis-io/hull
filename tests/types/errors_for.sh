#!/bin/sh
# Contributor helper: run tsc over types/hull and print only the errors that
# concern the given modules, so one module's declarations can be finished
# while others are still incomplete.
#   tests/types/errors_for.sh hull:db hull:kv ...
# An error counts when it is in types/hull/modules/<db>.d.ts or a fixture
# naming the module, or on a drift.ts line generated for the module.
# Run `make check-types` (or its first two steps) first so build/types/drift.ts exists.
cd "$(dirname "$0")/../.." || exit 1
TSC=tests/types/node_modules/.bin/tsc
out=$($TSC -p tests/types/tsconfig.json 2>&1)
for mod in "$@"; do
    file="types/hull/modules/$(echo "${mod#hull:}" | tr : .).d.ts"
    [ "$mod" = hull:app ] && file=types/hull/app.d.ts
    printf '%s\n' "$out" | grep -F "$file(" 
    printf '%s\n' "$out" | grep -F 'build/types/drift.ts(' | while IFS= read -r l; do
        ln=$(printf '%s' "$l" | sed -n 's/^build\/types\/drift.ts(\([0-9]*\),.*/\1/p')
        src=$(sed -n "${ln}p" build/types/drift.ts)
        case $src in
            *"// $mod."*|*"// $mod:"*|*"\"$mod\""*) printf '%s\n    %s\n' "$l" "$(printf '%s' "$src" | cut -c1-140)" ;;
        esac
    done
done
printf '%s\n' "$out" | grep -E '^tests/types/fixtures/|^types/hull/(core|globals|test|index)\.d\.ts' | sed 's/^/[shared] /' | head -50
