#!/bin/sh
# Phase 4c (docs/build_modularization.md): fail if the installable-feature
# registry in src/hull/commands/feature.c FEATURES[] drifts from the single
# source of truth in mk/feature.mk. Adding/removing a --with feature must be
# one edit; this catches the case where feature.c and the Makefile disagree.
#
# Args: "$1" = FEATURE_INSTALLABLE_STEMS, "$2" = FEATURE_EMBEDDED_STEMS.
set -eu
FC=src/hull/commands/feature.c

# The HL_FEATURE_INSTALLABLE rows (anchored on the `{ "name"` row start so the
# enum declaration line is not matched) must equal FEATURE_INSTALLABLE_STEMS.
c_inst=$(grep -E '\{ *"[a-z]+".*HL_FEATURE_INSTALLABLE,' "$FC" \
         | sed -E 's/^[[:space:]]*\{ *"([a-z]+)".*/\1/' | sort | tr '\n' ' ')
mk_inst=$(printf '%s\n' $1 | grep . | sort | tr '\n' ' ')
if [ "$c_inst" != "$mk_inst" ]; then
  echo "ERROR: feature.c installable features drifted from FEATURE_INSTALLABLE_STEMS" >&2
  echo "  feature.c FEATURES[]:      [$c_inst]" >&2
  echo "  mk/feature.mk registry:    [$mk_inst]" >&2
  echo "  -> reconcile src/hull/commands/feature.c and mk/feature.mk" >&2
  exit 1
fi

# Each HL_FEATURE_EMBEDDED row (the user-facing runtimes lua/js) must be a real
# embedded archive stem.
for f in $(grep -E '\{ *"[a-z]+".*HL_FEATURE_EMBEDDED,' "$FC" \
           | sed -E 's/^[[:space:]]*\{ *"([a-z]+)".*/\1/'); do
  case " $2 " in
    *" $f "*) ;;
    *) echo "ERROR: feature.c embedded feature '$f' is not in FEATURE_EMBEDDED_STEMS [$2]" >&2; exit 1 ;;
  esac
done

echo "check-feature-registry: OK (installable: $c_inst)"
