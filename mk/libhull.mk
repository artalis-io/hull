# mk/libhull.mk - libhull: the no-runtime embedding library (Phase L-1/L-2).
#
# libhull.a is the runtime-agnostic hardened core as a static archive for a
# native host to embed (no Lua/JS) + the C/Rust/Zig reference embedders
# (embed-smoke). Extracted verbatim from the root Makefile (build
# modularization). Self-contained: LIBHULL_OBJS references the base object
# lists (defined earlier) + MANIFEST_CORE_OBJ (libhull-only, moved here);
# the objects are built by the compile pattern rules below (rules are
# position-independent). Included at the original position.

# ── libhull: no-runtime embedding library (Phase L-1/L-2) ────────────
# The runtime-agnostic hardened core as a static archive, for a native
# host (C/Rust/Zig) that owns main() and drives the two-phase sandbox +
# the capability layer (no Lua/JS runtime, no app.main lifecycle). The
# host targets the stable ABI in <hull/embed.h> (embed.o, first object
# below) and never includes an internal Hull header.
# Includes ONLY runtime-free objects: embed.o (the hl_embed_* ABI),
# cap/* (fs/db/crypto/env/time/image/
# mime/wasm/gpu/blob/http/ws/smtp/body/audit), the async workers + async/
# net backend vtables, manifest.o (runtime-free struct + seal helpers, NOT
# the manifest_lua/js VM extractors), module registry/resolver, sandbox,
# signatures/release/SBOM, vfs/blob/cache/seal-arena, migrate, and the
# vendored libs (WAMR, mbedTLS, SQLite, tweetnacl, stb, pledge).
# Excludes: main/serve/entry (host owns main), CLI dispatch + commands,
# agent tooling, the build-tool VM (tool/compiler), BOTH runtimes
# (RT_OBJS/VEND_OBJS) + their manifest extractors, app_context,
# runtime_factory, static.c (HTTP-serving middleware), embedded stdlib.
# Link a host with: build/libhull.a $(KEEL_LIB) build/libhull.a -lm -lpthread
# (libhull.a repeated so GNU ld resolves the libhull<->Keel<->mbedTLS cycle).
MANIFEST_CORE_OBJ := $(BUILDDIR)/manifest.o
LIBHULL_OBJS := $(EMBED_OBJ) $(CAP_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) \
	$(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(MANIFEST_CORE_OBJ) \
	$(MODULE_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(SANDBOX_OBJ) \
	$(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) \
	$(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(MIGRATE_OBJ) \
	$(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) $(FS_UTIL_OBJ) \
	$(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(TLS_CLIENT_OBJ) $(TLS_TRANSPORT_OBJ) $(TLS_TRANSPORT_STUB_OBJ) $(CSP_OBJ) \
	$(SH_SEAL_ARENA_OBJ) $(SBOM_OBJ) $(WAMR_OBJS) $(MBEDTLS_OBJS) \
	$(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) \
	$(TWEETNACL_OBJ) $(STB_OBJ) $(PLEDGE_OBJS)

# Under the fat cosmocc driver, every object has a build/<dir>/.aarch64/<name>.o
# sibling, and a linked archive must ship a concomitant build/.aarch64/libhull.a.
# Mirror exactly how Keel builds .aarch64/libkeel.a: the per-object aarch64
# siblings, archived with the single-arch cosmo ar for each arch (plain cosmoar
# recurses into .aarch64/ and fails). `make CC=cosmocc` sets $(notdir CC)=cosmocc.
ifeq ($(notdir $(CC)),cosmocc)
  LIBHULL_COSMO_FAT  := 1
  LIBHULL_OBJS_ARM64 := $(foreach o,$(LIBHULL_OBJS),$(dir $(o)).aarch64/$(notdir $(o)))
endif

.PHONY: libhull
libhull: $(BUILDDIR)/libhull.a $(BUILDDIR)/libhull.a.sha256
$(BUILDDIR)/libhull.a: $(LIBHULL_OBJS) | $(BUILDDIR)
	@rm -f $@
ifeq ($(LIBHULL_COSMO_FAT),1)
	x86_64-unknown-cosmo-ar rcs $@ $(LIBHULL_OBJS)
	@mkdir -p $(BUILDDIR)/.aarch64
	@rm -f $(BUILDDIR)/.aarch64/libhull.a
	aarch64-unknown-cosmo-ar rcs $(BUILDDIR)/.aarch64/libhull.a $(LIBHULL_OBJS_ARM64)
	@echo "built $@ + .aarch64/libhull.a ($(words $(LIBHULL_OBJS)) objects, dual-arch)"
else
	$(AR) rcs $@ $(LIBHULL_OBJS)
	@echo "built $@ ($(words $(LIBHULL_OBJS)) objects)"
endif

# Checksum sidecar: the raw SHA-256 of the archive, one hex line. This is the
# value the signed release manifest (hull.sha256) carries for libhull.a, so a
# consumer can verify a downloaded archive offline against the Ed25519-signed
# manifest via hl_release_io_verify_local_asset.
$(BUILDDIR)/libhull.a.sha256: $(BUILDDIR)/libhull.a
	@$(SHA256CMD) $< | awk '{print $$1}' > $@
	@echo "libhull.a sha256: $$(cat $@)"

# embed-c-smoke: link the reference native host (examples/embed_c) against
# libhull.a alone — no Lua/JS runtime linked — and run it. Proves the core
# archive is genuinely runtime-free: an undefined-symbol pull from either
# runtime would fail the link here. The host drives the two-phase sandbox,
# capability-mediated fs I/O (incl. traversal rejection), crypto, the
# module registry, and platform identity, and exits non-zero on any
# capability failure.
#
# libhull.a is repeated after $(KEEL_LIB) because the two archives are
# mutually dependent (libhull's cap/release_io -> Keel's TLS -> libhull's
# mbedTLS). GNU ld resolves archives strictly left-to-right, so the second
# pass over libhull.a supplies the mbedTLS symbols Keel pulls in; macOS ld64
# resolves the cycle on its own and treats the repeat as a harmless no-op.
.PHONY: embed-c-smoke
embed-c-smoke: $(BUILDDIR)/libhull.a $(KEEL_LIB)
	$(CC) -std=c11 $(INCLUDES) -o $(BUILDDIR)/embed_c \
		examples/embed_c/main.c $(BUILDDIR)/libhull.a $(KEEL_LIB) $(BUILDDIR)/libhull.a -lm -lpthread
	@echo "── running embed_c (no-runtime host) ──"
	@$(BUILDDIR)/embed_c
	@echo "embed-c-smoke: OK"

# embed-rust-smoke / embed-zig-smoke: the non-C reference hosts (L-5). Each
# links libhull.a the same way embed_c does (archive repeated for the GNU-ld /
# lld cycle) and drives the hl_embed_* ABI from a foreign language. Both SKIP
# CLEANLY when the toolchain is absent (like the Playwright targets), so a
# plain `make check` on a box without cargo/zig is unaffected; CI installs the
# toolchains and runs them for real.
.PHONY: embed-rust-smoke
embed-rust-smoke: $(BUILDDIR)/libhull.a $(KEEL_LIB)
	@if ! command -v cargo >/dev/null 2>&1; then \
		echo "embed-rust-smoke: cargo not found, skipping"; \
	else \
		echo "── building + running embed_rust (no-runtime host) ──"; \
		HULL_LIBHULL_A="$(abspath $(BUILDDIR)/libhull.a)" \
		HULL_LIBKEEL_A="$(abspath $(KEEL_LIB))" \
		cargo run --quiet --release --manifest-path examples/embed_rust/Cargo.toml && \
		echo "embed-rust-smoke: OK"; \
	fi

.PHONY: embed-zig-smoke
embed-zig-smoke: $(BUILDDIR)/libhull.a $(KEEL_LIB)
	@if ! command -v zig >/dev/null 2>&1; then \
		echo "embed-zig-smoke: zig not found, skipping"; \
	else \
		echo "── building + running embed_zig (no-runtime host) ──"; \
		zig build-exe examples/embed_zig/main.zig -I$(INCDIR) -lc \
			-femit-bin=$(BUILDDIR)/embed_zig \
			$(BUILDDIR)/libhull.a $(KEEL_LIB) $(BUILDDIR)/libhull.a -lm -lpthread && \
		$(BUILDDIR)/embed_zig && \
		echo "embed-zig-smoke: OK"; \
	fi

# Run every reference embedder that has a toolchain available.
.PHONY: embed-smoke
embed-smoke: embed-c-smoke embed-rust-smoke embed-zig-smoke
