# mk/features/postgres.mk - PostgreSQL wire backend composable feature (hull build --with=postgres).
# Moved verbatim from the root Makefile (build modularization, Phase 1). A --with
# feature: sub-make target + archive rule, not embedded in hull, native only.

# ── PostgreSQL feature archive (composable feature: hull build --with=postgres) ──
# Bundles the pure-C Postgres v3 wire client (cap_db_postgres.o + cap_pg_conn.o +
# cap_pgwire.o, defining hl_db_backend_postgres) into ONE archive. Unlike DuckDB
# there is no vendored engine - it's ~4 KB of Hull's own code, so a straight
# bundle, no isolation. The backend references base crypto (SCRAM auth) + the
# shared tls_client (sslmode TLS); both resolve from the platform lib at compose,
# which build.lua wraps in --start-group because tls_client is not otherwise
# pulled by a DB-only app (only cap/smtp references it in the base). Native only.
# `feature-postgres` re-invokes make with HL_ENABLE_POSTGRES=1 so the wire objects
# (filtered out of a base build) are in scope.
feature-postgres:
	$(MAKE) $(BUILDDIR)/libhull_feature-postgres.a HL_ENABLE_POSTGRES=1
.PHONY: feature-postgres

$(BUILDDIR)/libhull_feature-postgres.a: $(BUILDDIR)/cap_db_postgres.o $(BUILDDIR)/cap_pg_conn.o $(BUILDDIR)/cap_pgwire.o | $(BUILDDIR)
	@rm -f $@
	$(AR) rcs $@ $(BUILDDIR)/cap_db_postgres.o $(BUILDDIR)/cap_pg_conn.o $(BUILDDIR)/cap_pgwire.o
	@echo "built $@ ($$(du -h $@ | cut -f1))"
