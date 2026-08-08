# mk/tests.mk - unit tests, MSan, fuzzing, and e2e.
#
# The test/verification RULE section: TEST_* vars, the per-suite test_* build
# rules, the msan/fuzz build variants, and the e2e-* phony targets. Extracted
# verbatim from the root Makefile (build modularization), included at the
# original position (after the base object lists + compile rules it depends
# on). TEST_COMMON_DEPS/LIBS are referenced later by the bench targets, which
# sit after this include so they resolve. Debug (a build mode) and self-build/
# reproducibility (dev targets) stay in the root Makefile.

# ── Tests ───────────────────────────────────────────────────────────

# Discover all test sources under tests/hull/
TEST_SRCS := $(shell find $(TESTDIR)/hull -name 'test_*.c')

# Filter test binaries based on RUNTIME selection
ifeq ($(RUNTIME),js)
  TEST_SRCS := $(filter-out %/test_lua.c,$(TEST_SRCS))
else ifeq ($(RUNTIME),lua)
  TEST_SRCS := $(filter-out %/test_js.c,$(TEST_SRCS))
endif

# Drop DB-dependent tests in pure-compute builds.
# test_js.c and test_lua.c exercise the full orchestration surface
# (including db.*) — they reference hl_db_backend_sqlite directly.
# Pure-compute builds skip them; the runtime sandbox is still covered
# by the smaller runtime-isolated tests.
ifeq ($(HL_ENABLE_DB),0)
  TEST_SRCS := $(filter-out \
      %/test_db.c %/test_db_backend.c %/test_db_select.c %/test_db_dynamic.c \
      %/test_js.c %/test_lua.c, \
      $(TEST_SRCS))
endif

# Drop the image codec test when the image subsystem is compiled out; it
# exercises hl_cap_image_* / stb directly and would fail to link.
ifeq ($(HL_ENABLE_IMAGE),0)
  TEST_SRCS := $(filter-out %/test_image.c,$(TEST_SRCS))
endif

# Flatten test paths to build/ binaries: tests/hull/cap/test_body.c → build/test_body
TEST_BINS := $(addprefix $(BUILDDIR)/,$(notdir $(basename $(TEST_SRCS))))

# Test objects need hull capability sources but NOT main.o or runtime objects
TEST_CAP_OBJS := $(CAP_OBJS)

# Shared link deps for all tests. RUNTIME_FACTORY_NONE_OBJ supplies the explicit
# empty hl_runtime_feature_factories() default: the base ships no weak default, so
# test binaries (which init runtimes directly, not via the hook) provide the empty
# here. No test links a real runtime-factory registry, so there is no conflict.
TEST_COMMON_DEPS := $(TEST_CAP_OBJS) $(RUNTIME_FACTORY_NONE_OBJ) $(ALLOC_OBJ) $(ASYNC_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(TLS_CLIENT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(SH_SEAL_ARENA_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(WAMR_OBJS) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(KEEL_LIB)
# SH_SEAL_ARENA_OBJ comes BEFORE $(KEEL_LIB) so Hull's instrumented
# copy resolves sh_seal_arena_* symbols first; Keel's copy stays in
# libkeel.a but the linker doesn't pull it (its symbols are already
# satisfied).  Required for MSan instrumentation visibility.
TEST_COMMON_LIBS := $(TEST_CAP_OBJS) $(RUNTIME_FACTORY_NONE_OBJ) $(ALLOC_OBJ) $(ASYNC_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(TLS_CLIENT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(SH_SEAL_ARENA_OBJ) $(WAMR_OBJS) $(MBEDTLS_OBJS) $(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(WGPU_LIB) $(WGPU_FRAMEWORKS) $(DUCKDB_LIBS) -lm -lpthread
# forkpty(3) is in libutil on glibc/musl Linux (used by
# tests/hull/cap/test_tui_lifecycle.c). macOS / BSD ship it inside
# libSystem so no extra flag is needed. Cosmopolitan does not provide
# libutil at all — gating on !COSMO keeps the cosmocc CI green; the
# TUI lifecycle test on cosmo already short-circuits via the
# HL_HAVE_FORKPTY=0 path.
ifeq ($(UNAME_S),Linux)
ifndef COSMO
  TEST_COMMON_LIBS += -lutil
endif
endif

# pgwire codec test: links the codec source directly. cap/pgwire.c is a
# self-contained parser gated out of CAP_OBJS until HL_ENABLE_POSTGRES, so
# the generic cap-test rule below (which relies on TEST_COMMON_LIBS) cannot
# resolve its symbols. Explicit rule wins over the pattern rule.
$(BUILDDIR)/test_pgwire: $(TESTDIR)/hull/cap/test_pgwire.c $(SRCDIR)/hull/cap/pgwire.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ \
		$(TESTDIR)/hull/cap/test_pgwire.c $(SRCDIR)/hull/cap/pgwire.c $(LDFLAGS)

# pgwire connection / DSN / handshake test: same rationale as test_pgwire.
# pg_conn.c + pgwire.c are gated out of CAP_OBJS until HL_ENABLE_POSTGRES.
PG_CRYPTO_OBJS := $(BUILDDIR)/cap_crypto.o $(BUILDDIR)/cap_crypto_hmac_mbedtls.o \
                  $(BUILDDIR)/cap_crypto_asym_mbedtls.o $(MBEDTLS_OBJS) $(TWEETNACL_OBJ)
# HL_PG_NO_TLS strips the Keel-dependent TLS transport: the tests drive the
# handshake over a plaintext socketpair, so they need neither tls_client.o nor
# Keel. sslmode parsing / SSLRequest negotiation stay covered (they are
# TLS-transport-independent).
$(BUILDDIR)/test_pg_conn: $(TESTDIR)/hull/cap/test_pg_conn.c $(SRCDIR)/hull/cap/pg_conn.c $(SRCDIR)/hull/cap/pgwire.c $(PG_CRYPTO_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -DHL_PG_NO_TLS $(INCLUDES) -I$(VENDDIR) -o $@ \
		$(TESTDIR)/hull/cap/test_pg_conn.c $(SRCDIR)/hull/cap/pg_conn.c $(SRCDIR)/hull/cap/pgwire.c \
		$(PG_CRYPTO_OBJS) $(LDFLAGS)

# Valkey/Redis RESP2/3 codec test: respwire.c is a self-contained parser gated
# out of CAP_OBJS until HL_ENABLE_VALKEY, so link it directly (explicit rule
# wins over the generic pattern rule).
$(BUILDDIR)/test_respwire: $(TESTDIR)/hull/cap/test_respwire.c $(SRCDIR)/hull/cap/respwire.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ \
		$(TESTDIR)/hull/cap/test_respwire.c $(SRCDIR)/hull/cap/respwire.c $(LDFLAGS)

# Valkey/Redis DSN parser test: valkey_conn.c's DSN part is self-contained (no
# socket/TLS yet) and gated out of CAP_OBJS until HL_ENABLE_VALKEY.
# -DHL_VALKEY_NO_TLS keeps the (Phase 1b) TLS transport out so the DSN test
# needs no Keel/mbedTLS link, mirroring test_pg_conn's -DHL_PG_NO_TLS.
$(BUILDDIR)/test_valkey_dsn: $(TESTDIR)/hull/cap/test_valkey_dsn.c $(SRCDIR)/hull/cap/valkey_conn.c $(SRCDIR)/hull/cap/respwire.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -DHL_VALKEY_NO_TLS $(INCLUDES) -I$(VENDDIR) -o $@ \
		$(TESTDIR)/hull/cap/test_valkey_dsn.c $(SRCDIR)/hull/cap/valkey_conn.c $(SRCDIR)/hull/cap/respwire.c $(LDFLAGS)

# Valkey/Redis connection: HELLO/AUTH handshake + RESP2 fallback + SELECT + a
# command round-trip over a socketpair. -DHL_VALKEY_NO_TLS drives the plaintext
# transport (no Keel/mbedTLS), mirroring test_pg_conn.
$(BUILDDIR)/test_valkey_conn: $(TESTDIR)/hull/cap/test_valkey_conn.c $(SRCDIR)/hull/cap/valkey_conn.c $(SRCDIR)/hull/cap/respwire.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -DHL_VALKEY_NO_TLS $(INCLUDES) -I$(VENDDIR) -o $@ \
		$(TESTDIR)/hull/cap/test_valkey_conn.c $(SRCDIR)/hull/cap/valkey_conn.c \
		$(SRCDIR)/hull/cap/respwire.c $(LDFLAGS)

# MySQL/MariaDB codec + DSN test: mysqlwire.c + mysql_conn.c (Phase 1b) are
# self-contained (no socket/TLS/crypto yet) and gated out of CAP_OBJS until
# HL_ENABLE_MYSQL, so link them directly. Explicit rule wins over the pattern.
# mysql_conn.c has the native_password scramble (cap/crypto -> SHA1), so link
# the crypto objects (reusing the PG set: cap_crypto + mbedTLS + tweetnacl).
# -DHL_MY_NO_TLS keeps mysql_conn.c free of Keel's KlTls (raw-socket transport)
# so the codec test needs no TLS link, mirroring test_pg_conn's -DHL_PG_NO_TLS.
$(BUILDDIR)/test_mysqlwire: $(TESTDIR)/hull/cap/test_mysqlwire.c $(SRCDIR)/hull/cap/mysqlwire.c $(SRCDIR)/hull/cap/mysql_conn.c $(PG_CRYPTO_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -DHL_MY_NO_TLS $(INCLUDES) -I$(VENDDIR) -o $@ \
		$(TESTDIR)/hull/cap/test_mysqlwire.c $(SRCDIR)/hull/cap/mysqlwire.c \
		$(SRCDIR)/hull/cap/mysql_conn.c $(PG_CRYPTO_OBJS) $(LDFLAGS)

# mysql connection / handshake test: drives hl_my_conn_start over a socketpair.
# Links mysql_conn.c (socket + native auth) + mysqlwire.c + the crypto objs.
# -DHL_MY_NO_TLS: the socketpair harness is plaintext, so drop the Keel/TLS dep.
$(BUILDDIR)/test_mysql_conn: $(TESTDIR)/hull/cap/test_mysql_conn.c $(SRCDIR)/hull/cap/mysql_conn.c $(SRCDIR)/hull/cap/mysqlwire.c $(PG_CRYPTO_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -DHL_MY_NO_TLS $(INCLUDES) -I$(VENDDIR) -o $@ \
		$(TESTDIR)/hull/cap/test_mysql_conn.c $(SRCDIR)/hull/cap/mysql_conn.c \
		$(SRCDIR)/hull/cap/mysqlwire.c $(PG_CRYPTO_OBJS) $(LDFLAGS)

# TUI cap-layer tests: cap/tui.c + tui_input.c + tui_width.c are filtered out of
# CAP_OBJS on the default (TUI-free) base — they live only in the composable
# feature archive. These tests call hl_cap_tui_* directly, so link the three TUI
# objects explicitly (built TUI-enabled via their target-specific CFLAGS above).
# A static-pattern rule wins over the generic cap-test pattern below. On a
# monolithic HL_ENABLE_TUI=1 build the objects are ALREADY in TEST_COMMON_LIBS
# (via CAP_OBJS), so the extra list is empty there — a doubled object on the
# link line is a multiple-definition error under GNU ld.
ifeq ($(HL_ENABLE_TUI),1)
TUI_CAP_TEST_OBJS :=
else
TUI_CAP_TEST_OBJS := $(BUILDDIR)/cap_tui.o $(BUILDDIR)/cap_tui_input.o $(BUILDDIR)/cap_tui_width.o
endif
$(BUILDDIR)/test_tui_lifecycle $(BUILDDIR)/test_tui_parser $(BUILDDIR)/test_tui_width: \
$(BUILDDIR)/test_%: $(TESTDIR)/hull/cap/test_%.c $(TUI_CAP_TEST_OBJS) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TUI_CAP_TEST_OBJS) $(TEST_COMMON_LIBS) $(LDFLAGS)

# Capability tests (tests/hull/cap/)
$(BUILDDIR)/test_%: $(TESTDIR)/hull/cap/test_%.c $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TEST_COMMON_LIBS) $(LDFLAGS)

# Top-level tests (tests/hull/)
$(BUILDDIR)/test_parse_size: $(TESTDIR)/hull/test_parse_size.c $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TEST_COMMON_LIBS)

# CFI death test — verifies -fsanitize=cfi-icall traps wrong-typed
# indirect calls.  Self-skips on non-CFI builds via __has_feature.
# No deps beyond libc.
$(BUILDDIR)/test_cfi: $(TESTDIR)/hull/test_cfi.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(LDFLAGS)

# CSP preset registry — tiny, no deps beyond <string.h>.
$(BUILDDIR)/test_csp: $(TESTDIR)/hull/test_csp.c $(CSP_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(CSP_OBJ)

# Embedding ABI — links the whole libhull.a the way a native host does,
# so this also link-tests the archive on every `make test`. Only the
# non-sealing surface runs in-process; the sealed path is embed-c-smoke.
$(BUILDDIR)/test_embed: $(TESTDIR)/hull/test_embed.c $(BUILDDIR)/libhull.a $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(BUILDDIR)/libhull.a $(KEEL_LIB) $(BUILDDIR)/libhull.a $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Arena lifetime helpers — mark/rewind/strdup/memdup over sh_arena.
# Needs only the alloc wrapper + the sh_arena bump allocator.
$(BUILDDIR)/test_arena: $(TESTDIR)/hull/test_arena.c $(ALLOC_OBJ) $(SH_ARENA_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(ALLOC_OBJ) $(SH_ARENA_OBJ)

# Event-loop thread-affinity assertions — needs only the helper TU + pthread.
# The death test self-skips unless HL_THREAD_AFFINITY_CHECKS is defined
# (DEBUG / MSAN / TSAN builds).
$(BUILDDIR)/test_thread_affinity: $(TESTDIR)/hull/test_thread_affinity.c $(THREAD_AFFINITY_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(THREAD_AFFINITY_OBJ) -lpthread

# Sealed arena — POSIX-only, no deps beyond libc + mmap/mprotect.
# Links Hull's locally-instrumented copy ($(SH_SEAL_ARENA_OBJ)) ahead
# of libkeel.a so MSan can see the init writes.
$(BUILDDIR)/test_seal_arena: $(TESTDIR)/hull/test_seal_arena.c $(SH_SEAL_ARENA_OBJ) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(SH_SEAL_ARENA_OBJ) $(KEEL_LIB)

# Manifest seal — exercises hl_manifest_seal end-to-end (round-trip
# + read-after-seal + fork+SIGSEGV write-after-seal death test).
# Links only manifest.o (the shared / runtime-free part) — manifest_lua
# and manifest_js are NOT needed since the fixture builds the manifest
# by hand, and pulling them in would force a QuickJS + Lua link.
$(BUILDDIR)/test_manifest_seal: $(TESTDIR)/hull/test_manifest_seal.c $(BUILDDIR)/manifest.o $(ALLOC_OBJ) $(SH_ARENA_OBJ) $(SH_SEAL_ARENA_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(BUILDDIR)/manifest.o $(ALLOC_OBJ) $(SH_ARENA_OBJ) $(SH_SEAL_ARENA_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(KEEL_LIB) -lpthread

# JS runtime test — needs QuickJS + JS runtime objects + manifest (JS-only to avoid Lua link deps)
$(BUILDDIR)/test_js: $(TESTDIR)/hull/runtime/js/test_js.c $(TEST_COMMON_DEPS) $(MANIFEST_JS_OBJ) $(MODULE_OBJ) $(CAP_TEST_JS_OBJ) $(STDLIB_FEATURE_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(STDLIB_RT_REGISTRY_OBJS) $(STDLIB_TOOLCHAIN_REGISTRY_O) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(RUNTIME_CACHE_COMMON_OBJ) $(JS_RT_OBJS) $(QJS_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(CAP_TEST_JS_OBJ) $(STDLIB_FEATURE_OBJ) $(JS_RT_OBJS) $(MANIFEST_JS_OBJ) $(MODULE_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(STDLIB_RT_REGISTRY_OBJS) $(STDLIB_TOOLCHAIN_REGISTRY_O) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(TLS_CLIENT_OBJ) $(RUNTIME_CACHE_COMMON_OBJ) $(ALLOC_OBJ) $(ASYNC_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(WAMR_OBJS) $(QJS_OBJS) \
		$(KEEL_LIB) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Lua runtime test — needs Lua + Lua runtime objects + manifest (Lua-only) + cap_tool + build_assets
$(BUILDDIR)/test_lua: $(TESTDIR)/hull/runtime/lua/test_lua.c $(TEST_COMMON_DEPS) $(CAP_TOOL_OBJ) $(CAP_TEST_LUA_OBJ) $(BUILD_ASSET_OBJ) $(BUILDDIR)/cmd_doctor.o $(BUILDDIR)/cmd_dev.o $(BUILDDIR)/compiler.o $(OBJ_EMIT_OBJ) $(LINKER_SYSTEM_OBJ) $(LINKER_LLD_OBJ) $(LINKER_ZIG_OBJ) $(BUNDLED_OBJS_OBJ) $(BUILDDIR)/tool.o $(BUILDDIR)/tool_orchestration.o $(BUILDDIR)/sandbox.o $(BUILDDIR)/sandbox_tool.o $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(STDLIB_FEATURE_OBJ) $(APP_CONTEXT_OBJ) $(APP_CONTEXT_RT_OBJ) $(MIGRATE_OBJ) $(MANIFEST_OBJ) $(MODULE_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(STDLIB_RT_REGISTRY_OBJS) $(STDLIB_TOOLCHAIN_REGISTRY_O) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(RUNTIME_CACHE_COMMON_OBJ) $(LUA_RT_OBJS) $(JS_RT_OBJS) $(LUA_OBJS) $(QJS_OBJS) $(RUNTIME_FACTORY_OBJ) $(RUNTIME_FACTORY_NONE_OBJ) $(STATIC_OBJ) $(TEST_RUNNER_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(PLEDGE_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(TEST_CAP_OBJS) $(CAP_TOOL_OBJ) $(CAP_TEST_LUA_OBJ) $(BUILD_ASSET_OBJ) $(BUILDDIR)/cmd_doctor.o $(BUILDDIR)/cmd_dev.o $(BUILDDIR)/compiler.o $(OBJ_EMIT_OBJ) $(LINKER_SYSTEM_OBJ) $(LINKER_LLD_OBJ) $(LINKER_ZIG_OBJ) $(BUNDLED_OBJS_OBJ) $(BUILDDIR)/tool.o $(BUILDDIR)/tool_orchestration.o $(BUILDDIR)/sandbox.o $(BUILDDIR)/sandbox_tool.o $(BUILDDIR)/cacert.o $(TLS_CLIENT_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(STDLIB_FEATURE_OBJ) $(APP_CONTEXT_OBJ) $(APP_CONTEXT_RT_OBJ) $(MIGRATE_OBJ) $(LUA_RT_OBJS) $(JS_RT_OBJS) $(MANIFEST_OBJ) $(MODULE_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(STDLIB_RT_REGISTRY_OBJS) $(STDLIB_TOOLCHAIN_REGISTRY_O) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(RUNTIME_CACHE_COMMON_OBJ) $(RUNTIME_FACTORY_OBJ) $(RUNTIME_FACTORY_NONE_OBJ) $(STATIC_OBJ) $(TEST_RUNNER_OBJ) $(ALLOC_OBJ) $(ASYNC_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(WAMR_OBJS) $(LUA_OBJS) $(QJS_OBJS) \
		$(KEEL_LIB) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(WGPU_LIB) $(WGPU_FRAMEWORKS) $(PLEDGE_OBJS) -lm -lpthread

# Tool hardening test — cap/tool.c compiled without runtime flags (self-contained C functions)
CAP_TOOL_NONE_OBJ := $(BUILDDIR)/cap_tool_none.o
$(CAP_TOOL_NONE_OBJ): $(SRCDIR)/hull/cap/tool.c | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -c -o $@ $<

# TOOLS_INSTALL_OBJ (pure-libc registry + hl_tools_lookup_path) is needed for
# tool-resolution symbols referenced by the compiler backend.
$(BUILDDIR)/test_tool: $(TESTDIR)/hull/cap/test_tool.c $(CAP_TOOL_NONE_OBJ) $(COMPILER_OBJ) $(TOOLS_INSTALL_OBJ) $(FS_UTIL_OBJ) $(BUILD_ASSET_OBJ) $(BUILDDIR)/cap_audit.o $(SH_JSON_OBJ) $(SH_ARENA_OBJ) | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(CAP_TOOL_NONE_OBJ) $(COMPILER_OBJ) $(TOOLS_INSTALL_OBJ) $(FS_UTIL_OBJ) $(BUILD_ASSET_OBJ) $(BUILDDIR)/cap_audit.o $(SH_JSON_OBJ) $(SH_ARENA_OBJ)

# Compiler vtable tests
COMPILER_TEST_DEPS := $(TEST_CAP_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACERT_OBJ) $(TLS_CLIENT_OBJ) $(WAMR_OBJS) $(MBEDTLS_OBJS) $(KEEL_LIB) $(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ)

$(BUILDDIR)/test_compiler: $(TESTDIR)/hull/compiler/test_compiler.c $(COMPILER_OBJ) $(TOOLS_INSTALL_OBJ) $(FS_UTIL_OBJ) $(CAP_TOOL_NONE_OBJ) $(BUILD_ASSET_OBJ) $(BUILDDIR)/cap_audit.o $(SH_JSON_OBJ) $(SH_ARENA_OBJ) | $(BUILDDIR)
	$(CC) $(filter-out -DHL_ENABLE_LUA -DHL_ENABLE_JS,$(CFLAGS)) $(INCLUDES) -I$(VENDDIR) -o $@ \
		$(TESTDIR)/hull/compiler/test_compiler.c \
		$(COMPILER_OBJ) $(TOOLS_INSTALL_OBJ) $(FS_UTIL_OBJ) \
		$(CAP_TOOL_NONE_OBJ) $(BUILD_ASSET_OBJ) \
		$(BUILDDIR)/cap_audit.o $(SH_JSON_OBJ) $(SH_ARENA_OBJ) -lm

# Command dispatcher test — needs full command set (symbol resolution for command table)
$(BUILDDIR)/test_dispatch: $(TESTDIR)/hull/commands/test_dispatch.c $(CMD_OBJS) $(SERVE_OBJ) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(TOOL_OBJ) $(SANDBOX_OBJ) $(SANDBOX_TOOL_OBJ) $(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(TEST_RUNNER_OBJ) $(RUNTIME_FACTORY_OBJ) $(RUNTIME_FACTORY_NONE_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(TLS_CLIENT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(CSP_OBJ) $(SBOM_OBJ) $(STDLIB_FEATURE_OBJ) $(APP_CONTEXT_OBJ) $(APP_CONTEXT_RT_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(TEST_COMMON_DEPS) $(RT_OBJS) $(VEND_OBJS) $(MBEDTLS_OBJS) $(MANIFEST_OBJ) $(MODULE_OBJ) $(BUILD_ASSET_OBJ) $(COMPILER_OBJ) $(OBJ_EMIT_OBJ) $(LINKER_SYSTEM_OBJ) $(LINKER_LLD_OBJ) $(LINKER_ZIG_OBJ) $(BUNDLED_OBJS_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(STDLIB_RT_REGISTRY_OBJS) $(STDLIB_TOOLCHAIN_REGISTRY_O) $(PLEDGE_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(CMD_OBJS) $(SERVE_OBJ) $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(TOOL_OBJ) $(SANDBOX_OBJ) $(SANDBOX_TOOL_OBJ) $(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(TEST_RUNNER_OBJ) $(RUNTIME_FACTORY_OBJ) $(RUNTIME_FACTORY_NONE_OBJ) $(STATIC_OBJ) $(MIGRATE_OBJ) $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(TLS_CLIENT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(CSP_OBJ) $(SBOM_OBJ) $(STDLIB_FEATURE_OBJ) $(APP_CONTEXT_OBJ) $(APP_CONTEXT_RT_OBJ) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) \
		$(TEST_CAP_OBJS) $(RT_OBJS) $(MANIFEST_OBJ) $(MODULE_OBJ) $(BUILD_ASSET_OBJ) $(COMPILER_OBJ) $(OBJ_EMIT_OBJ) $(LINKER_SYSTEM_OBJ) $(LINKER_LLD_OBJ) $(LINKER_ZIG_OBJ) $(BUNDLED_OBJS_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(STDLIB_REGISTRY_O) $(STDLIB_RT_REGISTRY_OBJS) $(STDLIB_TOOLCHAIN_REGISTRY_O) $(ALLOC_OBJ) $(ASYNC_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(COMPRESS_OBJ) $(MINIZ_OBJ) $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(WAMR_OBJS) $(VEND_OBJS) \
		$(KEEL_LIB) $(MBEDTLS_OBJS) $(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) $(TWEETNACL_OBJ) $(STB_OBJ) $(PLEDGE_OBJS) $(WGPU_LIB) $(WGPU_FRAMEWORKS) -lm -lpthread

# Signature verification test — needs crypto + app_entries_default + vfs.
# Override HL_PLATFORM_PUBKEY_HEX to the all-zeros placeholder for this
# test only: the fixture's create_test_package_sig generates a fresh
# local platform keypair on every run, so the production (gethull.dev)
# pinning check at signature.c §5 would always reject it. Both §5
# (per-app platform layer) and §5b (v0.1.3 gethull layer) treat the
# all-zeros sentinel as "no pinned key, skip pinning", which is exactly
# what the test wants. The override must apply to signature.c, not just
# the test TU, so we compile a test-specific signature object.
# Each test consumer also passes no_verify_platform=1 to
# hl_verify_startup as belt-and-suspenders.
$(BUILDDIR)/signature_testpk.o: src/hull/signature.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) \
		'-DHL_PLATFORM_PUBKEY_HEX="0000000000000000000000000000000000000000000000000000000000000000"' \
		-c -o $@ $<

$(BUILDDIR)/test_signature: $(TESTDIR)/hull/test_signature.c $(BUILDDIR)/signature_testpk.o $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(APP_ENTRIES_DEFAULT_OBJ) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(BUILDDIR)/signature_testpk.o $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) \
		$(RELEASE_OBJ) $(RELEASE_IO_OBJ) \
		$(APP_ENTRIES_DEFAULT_OBJ) $(TEST_COMMON_LIBS)

# Release manifest sign/verify test — needs release.c + crypto
$(BUILDDIR)/test_release: $(TESTDIR)/hull/test_release.c $(RELEASE_OBJ) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(RELEASE_OBJ) $(TEST_COMMON_LIBS)

# Tool registry + path helpers — standalone module, no runtime deps.
$(BUILDDIR)/test_tools_install: $(TESTDIR)/hull/test_tools_install.c $(TOOLS_INSTALL_OBJ) $(FS_UTIL_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(TOOLS_INSTALL_OBJ) $(FS_UTIL_OBJ)

# release_io/sbom/verify_self now hash via the cap layer's self-contained
# SHA-256 (hl_cap_crypto_sha256) instead of mbedtls_sha256, so these focused
# test binaries must link cap/crypto.o — plus its TweetNaCl (SHA-512/ed25519)
# dependency and the mbedTLS HMAC backend it references in HTTP builds.
CRYPTO_TEST_OBJS := $(BUILDDIR)/cap_crypto.o $(BUILDDIR)/cap_crypto_hmac_mbedtls.o $(TWEETNACL_OBJ)

# Shared release I/O helpers (platform id, SHA-256, manifest parse,
# atomic write). Skipped on HL_ENABLE_HTTP_CLIENT=0 builds where the
# helper module isn't compiled in.
ifneq ($(HL_ENABLE_HTTP_CLIENT),0)
$(BUILDDIR)/test_release_io: $(TESTDIR)/hull/test_release_io.c $(RELEASE_IO_OBJ) $(RELEASE_OBJ) $(CACERT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(CRYPTO_TEST_OBJS) $(MBEDTLS_OBJS) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(RELEASE_IO_OBJ) $(RELEASE_OBJ) $(CACERT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(CRYPTO_TEST_OBJS) $(MBEDTLS_OBJS) $(KEEL_LIB) -lm -lpthread
endif

# Verify-self helpers test. Reuses release_io.{c,h} for asset-name,
# checksum-line lookup, SHA-256, and self-path resolution. Same link
# dependencies as test_release_io.
ifneq ($(HL_ENABLE_HTTP_CLIENT),0)
$(BUILDDIR)/test_verify_self: $(TESTDIR)/hull/test_verify_self.c $(RELEASE_IO_OBJ) $(RELEASE_OBJ) $(CACERT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(CRYPTO_TEST_OBJS) $(MBEDTLS_OBJS) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(RELEASE_IO_OBJ) $(RELEASE_OBJ) $(CACERT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(CRYPTO_TEST_OBJS) $(MBEDTLS_OBJS) $(KEEL_LIB) -lm -lpthread
endif

# Platform-sig helpers — reuses release.c (sign/verify) +
# release_io.c (find_checksum), so the test pulls those plus their
# transitive crypto deps. Available on all builds.
ifneq ($(HL_ENABLE_HTTP_CLIENT),0)
$(BUILDDIR)/test_platform_sig: $(TESTDIR)/hull/test_platform_sig.c $(PLATFORM_SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(CACERT_OBJ) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(PLATFORM_SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TEST_COMMON_LIBS)
endif

# Static file serving test — needs static middleware + vfs + keel
$(BUILDDIR)/test_static: $(TESTDIR)/hull/test_static.c $(STATIC_OBJ) $(TEST_COMMON_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(STATIC_OBJ) $(TEST_COMMON_LIBS)

# VFS test — standalone module, no runtime deps
$(BUILDDIR)/test_vfs: $(TESTDIR)/hull/test_vfs.c $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(SH_SEAL_ARENA_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(SH_SEAL_ARENA_OBJ)

# Object emitter test — standalone (obj_emit.o + utest only). Validates the
# ELF/Mach-O/COFF app_registry serialization structurally; the link+run
# round-trip lives in e2e_compiler_free.sh.
$(BUILDDIR)/test_obj_emit: $(TESTDIR)/hull/test_obj_emit.c $(OBJ_EMIT_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(OBJ_EMIT_OBJ)

# SBOM test — exercises the data table + all four format functions +
# embedded-blob SHA-256 cache. Links against sbom.o + cacert.o + mbedTLS;
# nothing else. If SBOM accidentally pulls in other Hull subsystems,
# this link line will need to grow — that's the orthogonality canary.
$(BUILDDIR)/test_sbom: $(TESTDIR)/hull/test_sbom.c $(SBOM_OBJ) $(CACERT_OBJ) $(SH_JSON_OBJ) $(SH_ARENA_OBJ) $(CRYPTO_TEST_OBJS) $(MBEDTLS_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(SBOM_OBJ) $(CACERT_OBJ) $(SH_JSON_OBJ) $(SH_ARENA_OBJ) $(CRYPTO_TEST_OBJS) $(MBEDTLS_OBJS)

# Path-normalize test — standalone, exercises hl_path_normalize directly
# so a regression in the helper is caught here rather than only via the
# runtime module loaders that consume it.
$(BUILDDIR)/test_path_normalize: $(TESTDIR)/hull/test_path_normalize.c $(PATH_NORM_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(PATH_NORM_OBJ)

# Async backend tests — exercise the HlAsyncBackend vtable.
# test_async_backend covers whichever backend hl_async_backend() returns
# (keel on HTTP=1, poll on HTTP=0). test_async_backend_poll always
# pins the poll backend by name, so it runs on both build flavors.
# $(KEEL_LIB) is a real prerequisite (not just a recipe arg): without it a -j
# build can start the link before vendor/keel/libkeel.a is built and fail with
# "cannot find vendor/keel/libkeel.a".
$(BUILDDIR)/test_async_backend: $(TESTDIR)/hull/test_async_backend.c $(ASYNC_BACKEND_OBJS) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(ASYNC_BACKEND_OBJS) $(KEEL_LIB) -lm -lpthread

$(BUILDDIR)/test_async_backend_poll: $(TESTDIR)/hull/test_async_backend_poll.c $(ASYNC_BACKEND_OBJS) $(KEEL_LIB) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(ASYNC_BACKEND_OBJS) $(KEEL_LIB) -lm -lpthread

# Module registry — standalone, only links the registry object
$(BUILDDIR)/test_module_registry: $(TESTDIR)/hull/test_module_registry.c $(MODULE_REGISTRY_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(MODULE_REGISTRY_OBJ)

# Module resolver — needs the registry plus the manifest types
# cap_gpu_feature.o / cap_tui_feature.o supply the weak hl_gpu_feature_backends /
# hl_tui_feature_present the resolver now consults (composed-feature GPU + TUI
# caps). Both are base-resident, so always available.
$(BUILDDIR)/test_module_resolver: $(TESTDIR)/hull/test_module_resolver.c $(MODULE_OBJ) $(BUILDDIR)/cap_gpu_feature.o $(BUILDDIR)/cap_tui_feature.o | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< $(MODULE_OBJ) $(BUILDDIR)/cap_gpu_feature.o $(BUILDDIR)/cap_tui_feature.o

# CA bundle test — links against cacert.o and mbedTLS for parse verification
$(BUILDDIR)/test_cacert: $(TESTDIR)/hull/test_cacert.c $(CACERT_OBJ) $(MBEDTLS_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(VENDDIR) -o $@ $< \
		$(CACERT_OBJ) $(MBEDTLS_OBJS)

test: $(TEST_BINS)
	@echo "Running tests..."
	@pass=0; fail=0; total=0; \
	for t in $(TEST_BINS); do \
		total=$$((total + 1)); \
		echo "=== $$(basename $$t) ==="; \
		if HULL_QUIET_AOT=1 $$t; then \
			pass=$$((pass + 1)); \
		else \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "$$pass/$$total tests passed"; \
	if [ $$fail -gt 0 ]; then exit 1; fi

# ── MSan build (requires clang, Linux only) ────────────────────────
#
# Use MSAN=1 as an internal flag so that CFLAGS/QJS_CFLAGS etc. are set
# inside the Makefile (not on the command line), which avoids:
#  1. CFLAGS leaking into the Keel submodule build
#  2. Shell double-escaping mangling the CONFIG_VERSION string

ifdef MSAN
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
            -g -O1 -fsanitize=memory,undefined -fno-omit-frame-pointer \
            -D_DEFAULT_SOURCE -DHL_THREAD_AFFINITY_CHECKS
LDFLAGS  := -fsanitize=memory,undefined
# Vendor TUs: keep MSan (we still want shadow tracking for uninitialized
# reads escaping into Hull code) but drop UBSan. The vendored crypto and
# JS interpreters have well-known "technically UB but works on every
# target" patterns (left shift of negative values in tweetnacl, function-
# pointer casts in quickjs); UBSan flags them as runtime errors, fails
# the CI job, and tells us nothing about Hull's own code. Hull's own
# CFLAGS above still get -fsanitize=memory,undefined.
QJS_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer \
              -DCONFIG_VERSION=\"$(QJS_VERSION)\" -DCONFIG_BIGNUM -D_GNU_SOURCE
CFLAGS    += -DHL_QJS_VERSION=\"$(QJS_VERSION)\"
LUA_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer \
              -DLUA_USE_POSIX
SQLITE_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer \
                 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5
LOG_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer \
              -DLOG_USE_COLOR
SH_ARENA_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer
SH_JSON_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer
TWEETNACL_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer
STB_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer
# mbedTLS must be MSan-instrumented too: it writes to caller buffers
# (e.g. mbedtls_sha256 → uint8_t digest[32]). Without instrumentation
# MSan can't see those writes and flags every subsequent read of the
# caller buffer as use-of-uninitialized-value. (Hit by
# test_release_io's sha256_hex_empty.) Preserve the -I paths and
# -DMBEDTLS_CONFIG_FILE from the non-MSan defaults at line 219.
MBEDTLS_CFLAGS := -std=c11 -O1 -w -fsanitize=memory -fno-omit-frame-pointer \
                  -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/library -I$(MBEDTLS_DIR) \
                  -DMBEDTLS_CONFIG_FILE='"hull_config.h"'
# Re-add runtime defines (the := above clobbers earlier += additions)
ifeq ($(RUNTIME),js)
  CFLAGS += -DHL_ENABLE_JS
else ifeq ($(RUNTIME),lua)
  CFLAGS += -DHL_ENABLE_LUA
else
  CFLAGS += -DHL_ENABLE_JS -DHL_ENABLE_LUA
endif
ifeq ($(HL_ENABLE_WASM),1)
  CFLAGS += -DHL_ENABLE_WASM
endif
ifeq ($(HL_ENABLE_GPU),1)
  CFLAGS += -DHL_ENABLE_GPU -I$(VENDDIR)/wgpu
endif
ifeq ($(HL_ENABLE_SQLITE),1)
  CFLAGS += -DHL_ENABLE_SQLITE
endif
ifeq ($(HL_ENABLE_POSTGRES),1)
  CFLAGS += -DHL_ENABLE_POSTGRES
endif
ifeq ($(HL_ENABLE_DB),1)
  CFLAGS += -DHL_ENABLE_DB
endif
# Re-add HTTP server/client defines. The Makefile-level
# NET_BACKEND_SRCS gate above keys off $(HL_ENABLE_HTTP_SERVER),
# while async/poll.c's #ifndef HL_ENABLE_HTTP_SERVER stubs key off
# the C macro. If the two disagree the linker sees duplicate
# definitions of hl_net_op_suspend / hl_net_op_complete (net/keel.o
# vs. async/poll.o). Keep them aligned for sanitizer builds.
ifeq ($(HL_ENABLE_HTTP_SERVER),1)
  CFLAGS += -DHL_ENABLE_HTTP_SERVER
endif
ifeq ($(HL_ENABLE_HTTP_CLIENT),1)
  CFLAGS += -DHL_ENABLE_HTTP_CLIENT
endif
ifneq ($(HL_ENABLE_HTTP_ANY),0)
  CFLAGS += -DHL_ENABLE_HTTP
endif
# Re-add version string (the := above clobbers earlier += additions)
CFLAGS += -DHL_VERSION=\"$(HL_VERSION)\"

# Re-add header-dependency tracking (the := blocks above clobbered the
# DEPFLAGS additions from the standard CFLAGS section).
CFLAGS           += $(DEPFLAGS)
QJS_CFLAGS       += $(DEPFLAGS)
LUA_CFLAGS       += $(DEPFLAGS)
SQLITE_CFLAGS    += $(DEPFLAGS)
LOG_CFLAGS       += $(DEPFLAGS)
SH_ARENA_CFLAGS  += $(DEPFLAGS)
SH_JSON_CFLAGS   += $(DEPFLAGS)
TWEETNACL_CFLAGS += $(DEPFLAGS)
STB_CFLAGS       += $(DEPFLAGS)
endif

msan:
	$(MAKE) clean
	$(MAKE) -C $(KEEL_DIR) clean
	$(MAKE) -C $(KEEL_DIR) CC=clang \
		KEEL_TLS=mbedtls MBEDTLS_CONFIG_FILE=hull_config.h \
		KEEL_COMPRESS=miniz MINIZ_DIR=$(CURDIR)/$(MINIZ_DIR)
	$(MAKE) CC=clang MSAN=1 test

# ThreadSanitizer — validate the worker-pool / shared-state paths under a
# real race detector. Targeted at the suites that actually spin worker
# threads, rather than the whole test set, to keep the signal on the
# threading code and avoid TSan's cost on single-threaded suites:
#   - test_wasm           : WASM compute worker + the shared-segment vs
#                           in-flight-async-call path (cap/wasm.c +
#                           worker_wasm.c; home of the inflight_async fix)
#   - test_async_backend  : the Keel-backed thread-pool async backend
#   - test_async_backend_poll : the poll-based thread-pool async backend
# The db.async (worker_db.c) and gpu.async (worker_gpu.c) workers ride the
# same pool backends covered here; their cap-layer suites are
# single-threaded so they add no race coverage. TSAN=1 appends
# -fsanitize=thread to Hull's own TUs; vendor objects (WAMR, mbedTLS,
# keel.a) stay uninstrumented, which TSan tolerates.
TSAN_TESTS := test_wasm test_async_backend test_async_backend_poll
tsan:
	$(MAKE) clean
	$(MAKE) TSAN=1 $(addprefix $(BUILDDIR)/,$(TSAN_TESTS))
	@for t in $(TSAN_TESTS); do \
		echo "── TSan: $$t ──"; \
		TSAN_OPTIONS="halt_on_error=1" $(BUILDDIR)/$$t || exit 1; \
	done

# ── Fuzzing (libFuzzer + ASan/UBSan) ────────────────────────────────
# Mirrors vendor/keel/fuzz. Requires clang with the libFuzzer runtime.
#   Linux: make fuzz CC=clang
#   macOS: make fuzz CC=/opt/homebrew/opt/llvm/bin/clang   (brew install llvm)
# NOTE: Apple's clang does NOT ship the libFuzzer runtime
# (libclang_rt.fuzzer_osx.a), so `make fuzz` with the default cc fails on macOS
# with "library '...fuzzer_osx.a' not found". Use a Homebrew LLVM clang there.
# Each harness compiles its parser sources fresh under the fuzzer
# instrumentation — these parsers are small and self-contained, so no
# libhull_platform.a link is needed. Keel already fuzzes the HTTP /
# multipart / websocket / response parsers in its own tree; these cover
# Hull's own untrusted-input parsers.
# _DEFAULT_SOURCE matches the main build (see the Linux CFLAGS block): under
# strict -std=c11, glibc hides getaddrinfo / struct addrinfo behind it, so the
# pg_conn.c-linking fuzzers (fuzz_pg_dsn, fuzz_pg_rewrite) fail to compile on
# Linux without it. Harmless on macOS.
FUZZ_CFLAGS := -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -D_DEFAULT_SOURCE \
               -fno-omit-frame-pointer -Iinclude -I$(SH_JSON_DIR) -I$(SH_ARENA_DIR)
FUZZ_TIME ?= 60

fuzz/fuzz_sh_json: fuzz/fuzz_sh_json.c $(SH_JSON_DIR)/sh_json.c $(SH_ARENA_DIR)/sh_arena.c
	$(CC) $(FUZZ_CFLAGS) -o $@ $^

fuzz/fuzz_path_normalize: fuzz/fuzz_path_normalize.c $(SRCDIR)/hull/utils/path_normalize.c
	$(CC) $(FUZZ_CFLAGS) -o $@ $^

fuzz/fuzz_mime_sniff: fuzz/fuzz_mime_sniff.c $(SRCDIR)/hull/cap/mime.c
	$(CC) $(FUZZ_CFLAGS) -o $@ $^

# Host-allowlist matcher: glob + CIDR parsing over attacker-influenced patterns
# and hosts (roadmap §2.2 dynamic DB connections).
fuzz/fuzz_host_match: fuzz/fuzz_host_match.c $(SRCDIR)/hull/utils/host_match.c
	$(CC) $(FUZZ_CFLAGS) -o $@ $^

# Valkey/Redis RESP2/3 reply parser: the untrusted-server codec.
fuzz/fuzz_respwire: fuzz/fuzz_respwire.c $(SRCDIR)/hull/cap/respwire.c
	$(CC) $(FUZZ_CFLAGS) -o $@ $^

# Valkey/Redis DSN parser: percent-decoding + bounded field splitting over a
# user-supplied connection string. -DHL_VALKEY_NO_TLS keeps the (Phase 1b) TLS
# transport out so the pure-parser fuzzer needs no Keel/mbedTLS.
fuzz/fuzz_valkey_dsn: fuzz/fuzz_valkey_dsn.c $(SRCDIR)/hull/cap/valkey_conn.c $(SRCDIR)/hull/cap/respwire.c
	$(CC) $(FUZZ_CFLAGS) -DHL_VALKEY_NO_TLS -o $@ $^

# PostgreSQL wire-protocol reader: the untrusted-server parser (§1 Phase 2).
fuzz/fuzz_pgwire: fuzz/fuzz_pgwire.c $(SRCDIR)/hull/cap/pgwire.c
	$(CC) $(FUZZ_CFLAGS) -o $@ $^

# PostgreSQL DSN parser: percent-decoding + bounded field splitting (§1 Phase 2).
# HL_PG_NO_SCRAM keeps the pure-parser fuzzers free of the cap/crypto (mbedTLS)
# dependency that SCRAM adds to pg_conn.c; HL_PG_NO_TLS does the same for the
# Keel-backed TLS transport (Phase 3b.2).
fuzz/fuzz_pg_dsn: fuzz/fuzz_pg_dsn.c $(SRCDIR)/hull/cap/pg_conn.c $(SRCDIR)/hull/cap/pgwire.c
	$(CC) $(FUZZ_CFLAGS) -DHL_PG_NO_SCRAM -DHL_PG_NO_TLS -o $@ $^

# PostgreSQL placeholder rewriter: quote/comment-aware SQL scan (Phase 2).
fuzz/fuzz_pg_rewrite: fuzz/fuzz_pg_rewrite.c $(SRCDIR)/hull/cap/pg_conn.c $(SRCDIR)/hull/cap/pgwire.c
	$(CC) $(FUZZ_CFLAGS) -DHL_PG_NO_SCRAM -DHL_PG_NO_TLS -o $@ $^

# MySQL/MariaDB wire reader (cap/mysqlwire.c, §2.10 Phase 1b). Pure codec.
fuzz/fuzz_mysqlwire: fuzz/fuzz_mysqlwire.c $(SRCDIR)/hull/cap/mysqlwire.c
	$(CC) $(FUZZ_CFLAGS) -o $@ $^

# MySQL/MariaDB DSN parser (cap/mysql_conn.c). HL_MY_NO_AUTH strips the
# native_password scramble so the parser fuzzer stays free of cap/crypto.
fuzz/fuzz_mysql_dsn: fuzz/fuzz_mysql_dsn.c $(SRCDIR)/hull/cap/mysql_conn.c
	$(CC) $(FUZZ_CFLAGS) -DHL_MY_NO_AUTH -o $@ $^

fuzz: fuzz/fuzz_sh_json fuzz/fuzz_path_normalize fuzz/fuzz_mime_sniff fuzz/fuzz_host_match fuzz/fuzz_pgwire fuzz/fuzz_pg_dsn fuzz/fuzz_pg_rewrite fuzz/fuzz_mysqlwire fuzz/fuzz_mysql_dsn fuzz/fuzz_respwire fuzz/fuzz_valkey_dsn

# Time-boxed run over the seed corpora (what CI runs). FUZZ_TIME overrides.
fuzz-run: fuzz
	./fuzz/fuzz_sh_json fuzz/corpus_sh_json/ -max_total_time=$(FUZZ_TIME)
	./fuzz/fuzz_path_normalize fuzz/corpus_path_normalize/ -max_total_time=$(FUZZ_TIME)
	./fuzz/fuzz_mime_sniff fuzz/corpus_mime_sniff/ -max_total_time=$(FUZZ_TIME)
	./fuzz/fuzz_host_match fuzz/corpus_host_match/ -max_total_time=$(FUZZ_TIME)
	./fuzz/fuzz_pgwire fuzz/corpus_pgwire/ -max_total_time=$(FUZZ_TIME)
	./fuzz/fuzz_pg_dsn fuzz/corpus_pg_dsn/ -max_total_time=$(FUZZ_TIME)
	./fuzz/fuzz_pg_rewrite fuzz/corpus_pg_rewrite/ -max_total_time=$(FUZZ_TIME)
	./fuzz/fuzz_mysqlwire fuzz/corpus_mysqlwire/ -max_total_time=$(FUZZ_TIME)
	./fuzz/fuzz_mysql_dsn fuzz/corpus_mysql_dsn/ -max_total_time=$(FUZZ_TIME)
	./fuzz/fuzz_respwire fuzz/corpus_respwire/ -max_total_time=$(FUZZ_TIME)
	./fuzz/fuzz_valkey_dsn fuzz/corpus_valkey_dsn/ -max_total_time=$(FUZZ_TIME)

# ── E2E tests ──────────────────────────────────────────────────────

e2e: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e.sh

e2e-build:
	sh tests/e2e_build.sh

e2e-http: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_http.sh

# Runtime slim invariant: a produced single-runtime app drops the other
# interpreter (0 QuickJS in a lua app, 0 Lua VM in a js app). Locks the
# runtime-feature win against regression.
.PHONY: e2e-feature-runtime
e2e-feature-runtime: $(BUILDDIR)/hull
	sh tests/e2e_feature_runtime.sh

.PHONY: e2e-feature-wasm
e2e-feature-wasm: $(BUILDDIR)/hull
	sh tests/e2e_feature_wasm.sh

.PHONY: e2e-feature-sqlite
e2e-feature-sqlite: $(BUILDDIR)/hull
	sh tests/e2e_feature_sqlite.sh

.PHONY: e2e-feature-image
e2e-feature-image: $(BUILDDIR)/hull
	sh tests/e2e_feature_image.sh

.PHONY: e2e-feature-tls
e2e-feature-tls: $(BUILDDIR)/hull
	sh tests/e2e_feature_tls.sh

e2e-multipart: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_multipart.sh

e2e-attachment: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_attachment.sh

e2e-blob: $(BUILDDIR)/hull
	sh tests/e2e_blob.sh

e2e-jobs: $(BUILDDIR)/hull
	sh tests/e2e_jobs.sh

e2e-test-harness: $(BUILDDIR)/hull
	sh tests/e2e_test_harness.sh

e2e-named-connections: $(BUILDDIR)/hull
	sh tests/e2e_named_connections.sh

e2e-dynamic-connections: $(BUILDDIR)/hull
	sh tests/e2e_dynamic_connections.sh

e2e-hypermedia-photos-upload: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_hypermedia_photos_upload.sh

# Browser-driven E2E for HTMX example apps (chromium via Playwright).
# Catches things curl misses: CSS actually applies, htmx swaps fire,
# widget JS runs under the strict CSP preset. Skips cleanly when
# node/npm are absent so the target is safe to wire into CI.
# First run downloads ~150 MB into tests/.playwright/ (gitignored).
e2e-htmx-playwright: $(BUILDDIR)/hull
	sh tests/e2e_htmx_playwright.sh

# Same suite, but against `hull build` standalone binaries instead
# of `hull <app.lua>` (dev mode). Exercises the embedded-VFS code
# path — static files, templates, migrations, stdlib widget assets
# all loaded from the binary, not the filesystem. Needs hull built
# with EMBED_PLATFORM=1 (the make-rule below ensures it).
e2e-htmx-playwright-build: $(BUILDDIR)/hull $(BUILDDIR)/libhull_platform.a
	@if ! $(BUILDDIR)/hull doctor --json 2>/dev/null | grep -q '"hull_build":"ready"'; then \
	  echo "e2e-htmx-playwright-build: hull built without embedded platform."; \
	  echo "Run: make platform && make EMBED_PLATFORM=1"; \
	  exit 1; \
	fi
	MODE=build sh tests/e2e_htmx_playwright.sh

e2e-jwt-asym: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_jwt_asym.sh

e2e-oauth: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_oauth.sh

e2e-totp: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_totp.sh

e2e-auth-flows: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_auth_flows.sh

e2e-auth-flows-2fa: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_auth_flows_2fa.sh

e2e-auth-flows-hardening: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_auth_flows_hardening.sh

e2e-sign-in-events: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_sign_in_events.sh

e2e-sandbox: $(BUILDDIR)/hull
	sh tests/e2e_sandbox.sh

e2e-examples: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_examples.sh

# CLI-mode (app.main) examples: invoke + check stdout/exit. Distinct
# from e2e-examples which is HTTP-focused.
e2e-cli: $(BUILDDIR)/hull
	HULL_BIN=$(BUILDDIR)/hull sh tests/e2e_cli.sh

e2e-migrate: $(BUILDDIR)/hull
	sh tests/e2e_migrate.sh

# PostgreSQL backend end-to-end (needs Docker; builds its own POSTGRES hull).
e2e-postgres:
	sh tests/e2e_postgres.sh

# MySQL/MariaDB backend end-to-end (needs Docker; builds its own MYSQL hull).
e2e-mysql:
	sh tests/e2e_mysql.sh

e2e-templates: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_templates.sh

.PHONY: e2e-template-parity
e2e-template-parity: $(BUILDDIR)/hull
	sh tests/e2e_template_parity.sh

.PHONY: e2e-hex-parity
e2e-hex-parity: $(BUILDDIR)/hull
	sh tests/e2e_hex_parity.sh

.PHONY: e2e-client-ip-parity
e2e-client-ip-parity: $(BUILDDIR)/hull
	sh tests/e2e_client_ip_parity.sh

.PHONY: e2e-email
e2e-email: $(BUILDDIR)/hull
	sh tests/e2e_email.sh

.PHONY: e2e-uuid
e2e-uuid: $(BUILDDIR)/hull
	sh tests/e2e_uuid.sh

.PHONY: e2e-cache-module
e2e-cache-module: $(BUILDDIR)/hull
	sh tests/e2e_cache_module.sh

.PHONY: e2e-kv
e2e-kv: $(BUILDDIR)/hull
	sh tests/e2e_kv.sh

.PHONY: e2e-ratelimit-parity
e2e-ratelimit-parity: $(BUILDDIR)/hull
	sh tests/e2e_ratelimit_parity.sh

.PHONY: e2e-config-parity
e2e-config-parity: $(BUILDDIR)/hull
	sh tests/e2e_config_parity.sh

.PHONY: e2e-retry
e2e-retry: $(BUILDDIR)/hull
	sh tests/e2e_retry.sh

.PHONY: e2e-logx-parity
e2e-logx-parity: $(BUILDDIR)/hull
	sh tests/e2e_logx_parity.sh

.PHONY: e2e-naming-aliases
e2e-naming-aliases: $(BUILDDIR)/hull
	sh tests/e2e_naming_aliases.sh

.PHONY: e2e-csrf-cookie-fallback
e2e-csrf-cookie-fallback: $(BUILDDIR)/hull
	sh tests/e2e_csrf_cookie_fallback.sh

.PHONY: e2e-validate-parity
e2e-validate-parity: $(BUILDDIR)/hull
	sh tests/e2e_validate_parity.sh

.PHONY: e2e-token-interop
e2e-token-interop: $(BUILDDIR)/hull
	sh tests/e2e_token_interop.sh

e2e-agent: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_agent.sh

e2e-context: $(BUILDDIR)/hull
	sh tests/e2e_context.sh

e2e-mcp: $(BUILDDIR)/hull
	sh tests/e2e_mcp.sh

e2e-agent-api: $(BUILDDIR)/hull
	RUNTIME=$(RUNTIME) sh tests/e2e_agent_api.sh

e2e-compute: $(BUILDDIR)/hull
	sh tests/e2e_compute.sh

e2e-compute-dev: $(BUILDDIR)/hull
	sh tests/e2e_compute_dev.sh

# AOT artifact cache (requires wamrc — skipped cleanly when absent).
e2e-aot-cache: $(BUILDDIR)/hull
	sh tests/e2e_aot_cache.sh

# `hull cache list|prune|clear` + HULL_CACHE_DIR isolation.
e2e-cache: $(BUILDDIR)/hull
	sh tests/e2e_cache.sh

# Concurrent-writer stress test: N hull processes hammer the same
# cache root. Slow (~30s, spawns ~16 hull instances) — kept out of
# the default `make e2e` runs; CI invokes explicitly.
e2e-cache-concurrent: $(BUILDDIR)/hull
	sh tests/e2e_cache_concurrent.sh

# Run the cache e2e suite against a cosmopolitan-built hull. Slow
# (rebuilds platform + hull with cosmocc). CI invokes on a Linux
# x86_64 runner. The wrapper script verifies the binary is a cosmo
# APE, then delegates to e2e_cache.sh with HULL overridden.
e2e-cache-cosmo:
	@command -v cosmocc >/dev/null 2>&1 || { \
		echo "SKIP: cosmocc not on PATH"; exit 0; }
	$(MAKE) platform-cosmo
	$(MAKE) clean
	$(MAKE) CC=cosmocc EMBED_PLATFORM=cosmo -j8
	HULL=$(BUILDDIR)/hull sh tests/e2e_cache_cosmo.sh

e2e-compiler-free: $(BUILDDIR)/hull $(BUILDDIR)/libhull_platform.a
	sh tests/e2e_compiler_free.sh

e2e-linker: $(BUILDDIR)/hull $(BUILDDIR)/libhull_platform.a
	sh tests/e2e_linker.sh

# The zig linker backend (hull build --linker=zig). Runs on Linux x86_64 only
# (a foreign-target link needs a matching platform lib); skips elsewhere.
e2e-linker-zig: $(BUILDDIR)/hull $(BUILDDIR)/libhull_platform.a
	sh tests/e2e_linker_zig.sh

e2e-cross-build:
	sh tests/e2e_cross_build.sh

# Full musl support: builds hull under musl (Alpine) and runs emit-path compute
# + HTTP apps AND a fully static Tier B (--linker=lld-static) app. Self-re-execs
# into Docker on a non-musl host. No prereqs (it does its own musl build). See
# tests/e2e_musl.sh + docs/musl_build.md. (Supersedes the old e2e_tierb_musl.sh,
# which only static-linked a toy stub, not a real Hull app.)
e2e-musl:
	sh tests/e2e_musl.sh

# The musl CROSS-build loop (audit #4c): a glibc host builds the musl archive set
# (scripts/build_musl_platform.sh, Alpine), stages it + zig, then
# `hull build --target=x86_64-linux-musl --linker=zig` a compute app and RUNS the
# static musl result in Alpine. Linux x86_64 + Docker + zig; skips elsewhere. An
# EMBED_PLATFORM hull is needed to compose, so it depends on the platform lib.
e2e-musl-cross: $(BUILDDIR)/hull $(BUILDDIR)/libhull_platform.a
	sh tests/e2e_musl_cross.sh

# Assemble a self-contained musl static-link floor (crt*.o + libc.a + libgcc.a)
# for Tier B (`hull build --linker=lld-static`) - the contents of a
# `hull tools install libc-musl-<arch>` bundle. Run on a musl host (Alpine).
# DEST overrides the destination (default ~/.hull/tools/libc-musl-<arch>).
floor-musl:
	sh scripts/build_musl_floor.sh "$(if $(DEST),$(DEST),$(HOME)/.hull/tools/libc-musl-$(shell uname -m | sed -e s/arm64/aarch64/))"

# `hull build --flavor` MVP. Builds the pure-compute platform lib itself.
e2e-build-flavor: $(BUILDDIR)/hull
	sh tests/e2e_build_flavor.sh

# Composed-feature signature e2e (issue #114). Rebuilds the platform lib + hull
# TWICE with a test key, so it is slow (~minutes) and NOT part of the default
# e2e sweep - run it explicitly. It is the only local coverage of the gethull
# platform-sig layer (dev builds otherwise use the all-zeros placeholder).
e2e-composed-sig: $(BUILDDIR)/hull
	sh tests/e2e_composed_sig.sh
.PHONY: e2e-composed-sig

e2e-install:
	sh tests/e2e_install.sh

e2e-ca-bundle: $(BUILDDIR)/hull $(BUILDDIR)/test_cacert
	sh tests/e2e_ca_bundle.sh

e2e-update: $(BUILDDIR)/hull
	sh tests/e2e_update.sh

# Tools install (hermetic: HOME redirected, no network in fast path).
e2e-tools: $(BUILDDIR)/hull
	sh tests/e2e_tools.sh

# ── TUI e2e (smoke + interactive PTY-driven) ───────────────────────
#
# The interactive part shells out to the e2e_tui_drive helper which
# spawns the hull binary under a PTY and feeds it scripted input.
# Built whenever the hull binary carries TUI (HULL_HAS_TUI = a TUI-compiled
# base OR a TUI-free base that force-loads the feature archive for its own
# --tui commands); skipped only on a fully TUI-less build.

ifeq ($(HULL_HAS_TUI),1)
$(BUILDDIR)/e2e_tui_drive: $(TESTDIR)/e2e_tui_drive.c | $(BUILDDIR)
	$(CC) -std=c11 -Wall -Wextra -O2 -o $@ $<
E2E_TUI_DEPS := $(BUILDDIR)/hull $(BUILDDIR)/e2e_tui_drive
else
E2E_TUI_DEPS := $(BUILDDIR)/hull
endif

e2e-tui: $(E2E_TUI_DEPS)
	sh tests/e2e_tui.sh

hull-test-examples: $(BUILDDIR)/hull
	@for dir in examples/hello examples/rest_api examples/bench_db examples/auth \
	            examples/jwt_api examples/crud_with_auth examples/middleware examples/webhooks \
	            examples/entry examples/timers; do \
		echo "=== hull test $$dir ===" && \
		output=$$($(BUILDDIR)/hull test "$$dir" 2>&1; true) && \
		echo "$$output" && \
		if echo "$$output" | grep -qE "[0-9]+ failed"; then exit 1; fi; \
	done

