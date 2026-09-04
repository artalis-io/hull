# mk/features/mysql.mk - MySQL/MariaDB wire backend composable feature (hull build --with=mysql).
# Moved verbatim from the root Makefile (build modularization, Phase 1). A --with
# feature: sub-make target + archive rule, not embedded in hull, native only.

# ── MySQL/MariaDB feature archive (composable feature: hull build --with=mysql) ──
# Bundles the pure-C MySQL/MariaDB wire client (cap_db_mysql.o + cap_mysql_conn.o
# + cap_db_transport.o + cap_mysqlwire.o, defining hl_db_backend_mysql) into ONE
# archive. One backend serves both mysql:// and mariadb://. Same shape as
# feature-postgres: pure C, no vendored engine, references base crypto
# (native/caching_sha2 auth) + tls_client (sslmode) resolved from the platform lib
# at compose, so build.lua wraps them in --start-group (base_group).
# cap_db_transport.o additionally references Keel (KlConnectOp + KlEventCtx for
# the connect race), likewise resolved from the base at compose. cap_db_transport.o
# is the SHARED SQL-wire byte transport (docs/db_transport_extraction.md),
# intentionally bundled in BOTH archives; pull-by-symbol linking extracts it
# exactly once when both are composed. Native only.
# `feature-mysql` re-invokes make with HL_ENABLE_MYSQL=1 so the wire objects
# (filtered out of a base build) are in scope.
feature-mysql:
	$(MAKE) $(BUILDDIR)/libhull_feature-mysql.a HL_ENABLE_MYSQL=1
.PHONY: feature-mysql

$(BUILDDIR)/libhull_feature-mysql.a: $(BUILDDIR)/cap_db_mysql.o $(BUILDDIR)/cap_mysql_conn.o $(BUILDDIR)/cap_db_transport.o $(BUILDDIR)/cap_mysqlwire.o | $(BUILDDIR)
	@rm -f $@
	$(AR) rcs $@ $(BUILDDIR)/cap_db_mysql.o $(BUILDDIR)/cap_mysql_conn.o $(BUILDDIR)/cap_db_transport.o $(BUILDDIR)/cap_mysqlwire.o
	@echo "built $@ ($$(du -h $@ | cut -f1))"
