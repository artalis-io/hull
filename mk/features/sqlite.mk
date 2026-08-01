# mk/features/sqlite.mk - SQLite composable feature (docs/sqlite_feature.md).
#
# The sqlite FEATURE ARCHIVE (libhull_feature-sqlite.a: vendored SQLite engine
# + db backend + udf cap + sqlite agent), the per-runtime udf bridges
# (mod_db_udf -> libhull_feature-sqlite-<rt>.a), and BOTH embeds: the udf-bridge
# embed (EMBEDDED_SQLITE_RT_H) and the HL_APP_BASE_SQLITELESS core embed
# (EMBEDDED_SQLITE_H). Moved verbatim from four regions of the root Makefile
# (build modularization, Phase 1).
#
# Stays in the root Makefile: SQLITE_OBJ (vendored sqlite3.o, a base object in
# the hull-tool link), the mod_db_udf compile-gate ifneq, and the shared
# CFLAGS/BUILD_ASSET_OBJ lines referencing EMBEDDED_SQLITE_RT_H. The archive
# rule keeps its raw `ar rcs` for now; Phase 2 routes it through AR_FEATURE_LIB
# (fixing the TRUST_FEATURE_LIBS bypass, review finding F5).

# ── SQLite feature archive (SQLite as a composable feature, Phase B) ──
# docs/sqlite_feature.md. Bundles the vendored SQLite engine + the SQLite
# backend + UDF bridge + the SQLite-only agent introspection into ONE archive
# so the base can become SQLite-less (Phase B.2) and compose it back (Phase
# B.3). Unlike postgres/mysql (installable, off-by-default, filtered out of the
# base), SQLite is still in the default base today, so this target just packages
# the same objects into the archive as the additive first step; the base-flip +
# auto-compose land in later Phase B increments. The strong hl_db_feature_backends
# override is generated at compose (merges with any --with backends), not baked
# into the archive. agent/db.c references hl_agent_open_app_db + hl_agent_write_error
# from the base today; Phase B.2 splits agent/helpers.c so the opener moves here.
FEATURE_SQLITE_OBJS := $(SQLITE_OBJ) $(BUILDDIR)/cap_db.o \
                       $(BUILDDIR)/cap_db_sqlite.o \
                       $(BUILDDIR)/cap_db_udf.o $(BUILDDIR)/agent_db.o \
                       $(BUILDDIR)/agent_sql.o $(BUILDDIR)/agent_schema_diff.o \
                       $(BUILDDIR)/agent_db_open.o
feature-sqlite:
	$(MAKE) $(BUILDDIR)/libhull_feature-sqlite.a HL_ENABLE_SQLITE=1
.PHONY: feature-sqlite

$(BUILDDIR)/libhull_feature-sqlite.a: $(FEATURE_SQLITE_OBJS) | $(BUILDDIR)
	$(call AR_FEATURE_LIB,$(FEATURE_SQLITE_OBJS))

feature-sqlite-lua: $(BUILDDIR)/libhull_feature-sqlite-lua.a
.PHONY: feature-sqlite-lua
$(BUILDDIR)/libhull_feature-sqlite-lua.a: $(BUILDDIR)/lua_rt_mod_db_udf.o | $(BUILDDIR)
	$(call AR_FEATURE_LIB,$(BUILDDIR)/lua_rt_mod_db_udf.o)

feature-sqlite-js: $(BUILDDIR)/libhull_feature-sqlite-js.a
.PHONY: feature-sqlite-js
$(BUILDDIR)/libhull_feature-sqlite-js.a: $(BUILDDIR)/js_mod_db_udf.o | $(BUILDDIR)
	$(call AR_FEATURE_LIB,$(BUILDDIR)/js_mod_db_udf.o)

# Embed the per-runtime SQLite UDF bridges too (Phase C.2b). The runtime archive
# is SQLite-free; the default distributed hull composes the app's runtime bridge
# back whenever the app uses a udf-capable DB, with no install.
EMBEDDED_SQLITE_RT_H := $(BUILDDIR)/embedded_sqlite_rt.h
$(EMBEDDED_SQLITE_RT_H): $(BUILDDIR)/libhull_feature-sqlite-lua.a $(BUILDDIR)/libhull_feature-sqlite-js.a | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i $(BUILDDIR)/libhull_feature-sqlite-lua.a | sed 's/build_libhull_feature_sqlite_lua_a/hl_embedded_feature_sqlite_lua_a/g' | $(XXD_CONST_PIPE) >> $@
	@xxd -i $(BUILDDIR)/libhull_feature-sqlite-js.a  | sed 's/build_libhull_feature_sqlite_js_a/hl_embedded_feature_sqlite_js_a/g'   | $(XXD_CONST_PIPE) >> $@

# Phase D: when the app-build base is SQLite-less, embed the SQLite ENGINE
# archive (cap/db_sqlite + vendored sqlite3 + FTS5 + udf cap + sqlite agent) so a
# db app auto-composes it with no `hull feature install sqlite`. Mirrors the WASM
# core embed. Only pulled when HL_APP_BASE_SQLITELESS=1 (else the engine is
# in-base and this would be ~2 MB of dormant bloat).
ifeq ($(HL_APP_BASE_SQLITELESS),1)
EMBEDDED_SQLITE_H := $(BUILDDIR)/embedded_sqlite.h
$(EMBEDDED_SQLITE_H): $(BUILDDIR)/libhull_feature-sqlite.a | $(BUILDDIR)
	@echo "/* Auto-generated - do not edit */" > $@
	@xxd -i $(BUILDDIR)/libhull_feature-sqlite.a | sed 's/build_libhull_feature_sqlite_a/hl_embedded_feature_sqlite_a/g' | $(XXD_CONST_PIPE) >> $@
CFLAGS += -DHL_BUILD_EMBEDDED_SQLITE
$(BUILD_ASSET_OBJ): $(EMBEDDED_SQLITE_H)
endif
