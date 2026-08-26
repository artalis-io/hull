#!/bin/sh
# (docs/build_modularization.md): fail if the installable-feature
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

# T4e: every INSTALLABLE `--with` feature must have a FEATURE_SPECS entry - the
# codegen source of truth `hull build --with=<name>` reads to compose it (backend
# symbol, vtable type, hook name, extra link libs). Without it, `--with=<name>`
# would resolve the archive but emit no feature_registry.c to wire it into the
# base. FEATURE_SPECS is a SUPERSET (it also carries the mandatory `sqlite`
# default backend, which is not a `--with` feature), so the check is directional:
# installable stems must be a SUBSET of FEATURE_SPECS keys.
FS=stdlib/cli/lua/hull/feature_specs.lua
# Top-level keys only: exactly 4 leading spaces (direct children of `return {`).
# A looser `[[:space:]]+` would also match nested sub-tables like the 8-space
# `libs = {` inside the gpu/tui specs.
fs_keys=$(grep -E '^    [a-z][a-z_-]* = \{' "$FS" \
          | sed -E 's/^    ([a-z][a-z_-]*) =.*/\1/' | sort | tr '\n' ' ')
for f in $1; do
  case " $fs_keys " in
    *" $f "*) ;;
    *) echo "ERROR: installable feature '$f' has no FEATURE_SPECS entry in $FS" >&2
       echo "  FEATURE_SPECS keys: [$fs_keys]" >&2
       echo "  -> add a '$f = { ... }' block so 'hull build --with=$f' can compose it" >&2
       exit 1 ;;
  esac
done

echo "check-feature-registry: OK (installable: $c_inst; FEATURE_SPECS: $fs_keys)"
