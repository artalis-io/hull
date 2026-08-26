# mk/flags.mk - HTTP / DB / composable-feature config flags.
#
# Extracted verbatim from the root Makefile in the build modularization
# (docs/build_modularization.md). Included at the original position
# so the CFLAGS += accumulation order is preserved exactly: the HL_ENABLE_* -D
# macros and the derived HL_ENABLE_HTTP_ANY / HL_LINK_TLS gates depend on it.

# ── HTTP server / client — config flags ─────────────────────────────
#
# Declared here (early) because the Keel + mbedTLS sections below gate
# on $(HL_ENABLE_HTTP_ANY). Full prose docs are repeated at line 195
# (where they used to live) so anyone scrolling the build flags table
# also finds them.

HL_ENABLE_HTTP ?= 1

# Back-compat: HL_ENABLE_HTTP=0 forces both off; otherwise honour the
# granular flag defaults (both ?= 1 below).
ifeq ($(HL_ENABLE_HTTP),0)
HL_ENABLE_HTTP_SERVER ?= 0
HL_ENABLE_HTTP_CLIENT ?= 0
endif
HL_ENABLE_HTTP_SERVER ?= 1
HL_ENABLE_HTTP_CLIENT ?= 1

# CFLAGS macros: granular always defined; HL_ENABLE_HTTP (the legacy
# "any HTTP at all" gate) defined when either is on.
ifeq ($(HL_ENABLE_HTTP_SERVER),1)
CFLAGS += -DHL_ENABLE_HTTP_SERVER
endif
ifeq ($(HL_ENABLE_HTTP_CLIENT),1)
CFLAGS += -DHL_ENABLE_HTTP_CLIENT
endif
ifeq ($(HL_ENABLE_HTTP_SERVER)$(HL_ENABLE_HTTP_CLIENT),00)
HL_ENABLE_HTTP_ANY := 0
else
HL_ENABLE_HTTP_ANY := 1
CFLAGS += -DHL_ENABLE_HTTP
endif

# ── Database backend flags (resolved early) ─────────────────────────
# The granular SQLite / PostgreSQL flags are resolved here, ahead of the
# Keel + mbedTLS sections below, because those link gates now extend to
# PostgreSQL: its TLS transport and SCRAM auth need
# Keel's KlTls and mbedTLS. The -D macros, SQLITE_OBJ gate, and the derived
# HL_ENABLE_DB umbrella stay in the DB section further down. Back-compat:
# HL_ENABLE_DB=0 pins both granular flags off.
ifeq ($(HL_ENABLE_DB),0)
HL_ENABLE_SQLITE   ?= 0
HL_ENABLE_POSTGRES ?= 0
HL_ENABLE_MYSQL    ?= 0
HL_ENABLE_DUCKDB   ?= 0
endif
HL_ENABLE_SQLITE   ?= 1
HL_ENABLE_POSTGRES ?= 0
HL_ENABLE_MYSQL    ?= 0
HL_ENABLE_DUCKDB   ?= 0

# HL_SQLITE_FEATURE=1 builds a SQLite-as-a-composable-feature base
# (docs/sqlite_feature.md): SQLite leaves the base object set (composed
# back from libhull_feature-sqlite.a) but the DB CORE stays on. It forces
# HL_ENABLE_SQLITE off here; the umbrella below keeps HL_ENABLE_DB on so the
# vtable + selector + generic db.* caps + weak hl_db_feature_backends remain,
# with zero compiled backend. Orthogonal to the postgres/mysql flags (those may
# still be composed alongside). Native only; the default (0) is unchanged.
HL_SQLITE_FEATURE  ?= 0
ifeq ($(HL_SQLITE_FEATURE),1)
override HL_ENABLE_SQLITE := 0
endif

# HL_TLS_FEATURE=1 builds a TLS-as-a-composable-feature base (docs/tls_feature.md,
# a2): the vendored mbedTLS + the mbedTLS-consuming TUs (cap_crypto_{hmac,asym}
# _mbedtls.o, tls_client.o, tls_transport.o) leave the base object set, composed
# back from libhull_feature-tls.a. Keel + the HTTP server/event loop stay
# (HL_ENABLE_HTTP unchanged), as do the crypto weak defaults + tls_transport_stub;
# Keel's own tls_mbedtls.o then dead-strips from a TLS-less app link (nothing
# references kl_tls_*). Used only by the TLSLESS_PLATFORM_LIB sub-build; the hull
# binary + a plain `make` are unaffected. Native only. Default 0.
HL_TLS_FEATURE     ?= 0

# HL_KEEL_FEATURE=1 builds a Keel-less app-build base (docs/keel_feature.md):
# the base uses the Keel-free serve_cli.o app-entry (weak hull_serve)
# instead of serve.o, and drops async/keel.c (the Keel event loop), keeping only
# the weak poll backend. serve.o + async/keel.c (the strong hull_serve +
# hl_async_backend overrides) compose back in the whole-archived http feature on
# needs_http. A compute app references no kl_* and links Keel-free; libkeel stays
# available (merged in the base .a, pulled on-demand by a composed serve.o).
# Used only by the KEELLESS app-build base; the hull binary + plain `make`
# unaffected. Native only. Default 0.
HL_KEEL_FEATURE    ?= 0
# Surface the flag to C so async/poll.c compiles its weak net-backend stubs on a
# Keel-less base (HTTP_SERVER stays 1, so the plain #ifndef would miss it).
ifeq ($(HL_KEEL_FEATURE),1)
CFLAGS += -DHL_KEEL_FEATURE
endif

# HL_APP_BASE_SQLITELESS=1 (docs/sqlite_feature.md): the distributed
# hull embeds a SQLite-LESS platform lib as the app-build base (built in a
# HL_SQLITE_FEATURE=1 sub-build) plus the SQLite engine archive, so a stock
# `hull build` produces SQLite-DROPPING apps (a db-free app links zero sqlite3.*;
# a db app auto-composes the engine via the nm-probe gate). The hull
# binary ITSELF stays SQLite-full (it links SQLITE_OBJ for its own toolchain:
# hull test / agent). Only the EMBED_PLATFORM path honours this; a plain dev
# `make` is unaffected. Default 0 so the default distributed behaviour is
# unchanged until release.yml opts in. Cosmo ignores it (SQLite stays in-base).
HL_APP_BASE_SQLITELESS ?= 0

# HL_APP_BASE_TLSLESS=1 (docs/tls_feature.md, a2): the same idea for TLS. The
# distributed hull embeds a TLS-LESS platform lib as the app-build base (built in
# an HL_TLS_FEATURE=1 sub-build) plus libhull_feature-tls.a, so a stock `hull
# build` produces TLS-DROPPING apps (a plaintext app links zero mbedTLS; an HTTPS
# / net-DB app auto-composes the TLS feature via the needs_tls gate). The hull
# binary ITSELF stays TLS-full (its own `hull update` needs HTTPS). Composes with
# HL_APP_BASE_SQLITELESS (both -> a combined sub-build). Only the EMBED_PLATFORM
# path honours it; a plain dev `make` is unaffected. Cosmo ignores it. Default 0.
HL_APP_BASE_TLSLESS    ?= 0

# Keel (KlTls) + mbedTLS are linked when an HTTP half OR PostgreSQL OR MySQL OR
# Valkey is enabled (MySQL's caching_sha2_password full-auth + ed25519, and
# Valkey's rediss:// TLS, need the shared TLS client). HTTP still owns the
# -DHL_ENABLE_HTTP macro (above), so a DB/KV-only build links the TLS stack
# without activating HTTP code.
ifeq ($(HL_ENABLE_HTTP_ANY)$(HL_ENABLE_POSTGRES)$(HL_ENABLE_MYSQL)$(HL_ENABLE_VALKEY),0000)
HL_LINK_TLS := 0
else
HL_LINK_TLS := 1
endif
