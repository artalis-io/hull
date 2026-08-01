# mk/vendor/duckdb.mk - DuckDB static OLAP libs config (native-only,
# side-loaded). Extracted verbatim. (The --with archive rule is mk/features/duckdb.mk.)

# ── DuckDB (side-loaded, statically-linked OLAP backend) ────────────
#
# Enable with: make HL_ENABLE_DUCKDB=1
#   - Auto-detects vendor/duckdb/libduckdb_static.a if present
#   - Or specify: make HL_ENABLE_DUCKDB=1 DUCKDB_LIB_DIR=/path/to/libs
#   - Fetch automatically: make fetch-duckdb && make HL_ENABLE_DUCKDB=1
#
# The -DHL_ENABLE_DUCKDB macro + -I are emitted in the DB section above; this
# block resolves the archive set to link. DuckDB is a large C++ static library,
# so this is an opt-in side variant (the default `hull` rejects duckdb:// with
# the reserved-scheme hint). Not compatible with Cosmopolitan.

ifeq ($(HL_ENABLE_DUCKDB),1)
  ifdef COSMO
    $(error DuckDB is not compatible with Cosmopolitan builds)
  endif
  # libduckdb_static.a embeds its OWN (different-version) mbedTLS, which would
  # collide with Hull's. `make fetch-duckdb` isolates it by renaming DuckDB's
  # bundled mbedtls_/psa_ symbols to a private hlduck_ prefix (objcopy
  # --redefine-syms), so DuckDB and Hull's mbedTLS coexist in one binary and
  # DuckDB works alongside the full HTTP/TLS stack. See
  # docs/duckdb_backend_design.md §3.4.
  ifndef DUCKDB_LIB_DIR
    ifneq (,$(wildcard $(VENDDIR)/duckdb/libduckdb_static.a))
      DUCKDB_LIB_DIR := $(VENDDIR)/duckdb
    else
      $(error HL_ENABLE_DUCKDB=1 requires the DuckDB static libs. Run: make fetch-duckdb)
    endif
  endif
  # Proven link set (v1.5.4): the core static lib + the five default extensions
  # + their generated loader (which references those five, so none can be
  # dropped) + the third-party dep archives. Deliberately OMITS
  # libduckdb_mbedtls.a (unused by the default set) and the benchmark-only
  # tpch/tpcds extensions. See docs/duckdb_backend_design.md.
  DUCKDB_ARCHIVES := \
      $(DUCKDB_LIB_DIR)/libduckdb_static.a \
      $(DUCKDB_LIB_DIR)/libcore_functions_extension.a \
      $(DUCKDB_LIB_DIR)/libparquet_extension.a \
      $(DUCKDB_LIB_DIR)/libjson_extension.a \
      $(DUCKDB_LIB_DIR)/libicu_extension.a \
      $(DUCKDB_LIB_DIR)/libautocomplete_extension.a \
      $(DUCKDB_LIB_DIR)/libduckdb_generated_extension_loader.a \
      $(DUCKDB_LIB_DIR)/libduckdb_zstd.a \
      $(DUCKDB_LIB_DIR)/libduckdb_miniz.a \
      $(DUCKDB_LIB_DIR)/libduckdb_yyjson.a \
      $(DUCKDB_LIB_DIR)/libduckdb_re2.a \
      $(DUCKDB_LIB_DIR)/libduckdb_hyperloglog.a \
      $(DUCKDB_LIB_DIR)/libduckdb_utf8proc.a \
      $(DUCKDB_LIB_DIR)/libduckdb_fastpforlib.a \
      $(DUCKDB_LIB_DIR)/libduckdb_pg_query.a \
      $(DUCKDB_LIB_DIR)/libduckdb_skiplistlib.a \
      $(DUCKDB_LIB_DIR)/libduckdb_fmt.a \
      $(DUCKDB_LIB_DIR)/libduckdb_fsst.a
  # These archives reference each other circularly (the loader -> extensions ->
  # core, and back). GNU ld resolves that only within a --start-group; macOS
  # ld64 resolves regardless of order, so no group there. DuckDB is C++, so pull
  # in the C++ runtime (+ libdl for its extension machinery on Linux).
  ifeq ($(UNAME_S),Darwin)
    DUCKDB_LIBS := $(DUCKDB_ARCHIVES) -lc++
  else
    DUCKDB_LIBS := -Wl,--start-group $(DUCKDB_ARCHIVES) -Wl,--end-group -lstdc++ -ldl
  endif
else
  DUCKDB_LIBS :=
endif

