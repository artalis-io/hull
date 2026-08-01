# mk/features/duckdb.mk - DuckDB OLAP backend composable feature (hull build --with=duckdb).
# Moved verbatim from the root Makefile (build modularization, Phase 1). A --with
# feature: sub-make target + archive rule, not embedded in hull, native only.

# ── DuckDB feature archive (composable feature: hull build --with=duckdb) ──
# libhull_feature-duckdb.a bundles the DuckDB backend object (cap_db_duckdb.o,
# which defines hl_db_backend_duckdb) + the isolated DuckDB static libs into ONE
# self-contained archive. A build composes it in rather than compiling DuckDB
# into the base platform lib -- the feature model, see docs/features_and_flavors.md.
#
# Combining the archives is deliberate: it makes the DuckDB engine's circular
# refs (loader <-> extensions <-> core) INTRA-archive, so the composing link
# needs no --start-group (ld iterates a single archive's members to a fixed
# point). Each source archive is extracted into its own subdir first, because
# distinct archives can share a member name (e.g. two `utils.o`); collecting
# them by subdir keeps both -- ar tolerates duplicate member names in the output
# and ld pulls whichever resolves a needed symbol via the archive index.
# Native only (DuckDB is not cosmo-compatible). `feature-duckdb` re-invokes make
# with HL_ENABLE_DUCKDB=1 so DUCKDB_ARCHIVES + cap_db_duckdb.o are in scope.
feature-duckdb:
	$(MAKE) $(BUILDDIR)/libhull_feature-duckdb.a HL_ENABLE_DUCKDB=1
.PHONY: feature-duckdb

$(eval $(call define-feature-bundle,duckdb,$(BUILDDIR)/cap_db_duckdb.o,DUCKDB_ARCHIVES))
