# mk/features/valkey.mk - Valkey/Redis KV backend composable feature
# (hull build --with=valkey). The FIRST non-SQL connection feature: it fills the
# hl_kv_feature_backends hook (its own seam), NOT hl_db_feature_backends, so it
# composes side by side with the DB features.
#
# Bundles the pure-C RESP codec + connection + HlKvBackend vtable
# (cap_valkey.o + cap_valkey_conn.o + cap_respwire.o, defining
# hl_kv_backend_valkey) into ONE archive. Pure C, no vendored engine. The backend
# references base crypto (native/SCRAM-ish auth) + tls_client (rediss sslmode)
# resolved from the platform lib at compose, so build.lua wraps them in
# --start-group (base_group). Native only.
#
# Deliberately OMITS cap_valkey_register.o (the strong hl_kv_feature_backends
# self-registration used only by the compiled-in dev/test base): the generated
# feature_registry.c is the sole hook provider at compose, so there is never a
# duplicate symbol. `feature-valkey` re-invokes make with HL_ENABLE_VALKEY=1 so
# the wire objects (filtered out of a base build) are in scope.
feature-valkey:
	$(MAKE) $(BUILDDIR)/libhull_feature-valkey.a HL_ENABLE_VALKEY=1
.PHONY: feature-valkey

$(BUILDDIR)/libhull_feature-valkey.a: $(BUILDDIR)/cap_valkey.o $(BUILDDIR)/cap_valkey_conn.o $(BUILDDIR)/cap_respwire.o | $(BUILDDIR)
	@rm -f $@
	$(AR) rcs $@ $(BUILDDIR)/cap_valkey.o $(BUILDDIR)/cap_valkey_conn.o $(BUILDDIR)/cap_respwire.o
	@echo "built $@ ($$(du -h $@ | cut -f1))"
