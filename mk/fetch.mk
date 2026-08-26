# mk/fetch.mk - vendored/embedded asset refresh targets (maintenance only).
#
# Network-fetch phonies that re-download + checksum the assets committed under
# vendor/ and stdlib/, run by a maintainer on demand. NONE are part of `make`
# (no build target depends on them) - they are kept out of the build graph
# deliberately, so this file holds targets only, no object/link wiring.
#
#   make fetch-ca-bundle        Mozilla CA bundle (curl.se) -> vendor/cacert/
#   make fetch-pwned-blocklist  SecLists top-10K -> stdlib/.../_pwned_blocklist.*
#   make fetch-htmx / fetch-pico / fetch-htmx-pico   hull init --profile htmx assets
#   make fetch-unicode          Unicode EAW/UCD -> vendor/unicode/eaw.h (via gen.lua)
#
# Config defined before this include and referenced here: CACERT_* (the CA-bundle
# embed section, which stays in root because the embed rule is build-graph),
# UNICODE_* (mk/vendor/unicode.mk), CURL_RETRY (root). The HTMX_*/PICO_*/PWNED_*
# config is fetch-only and lives here with its target.

.PHONY: fetch-ca-bundle
fetch-ca-bundle:
	@mkdir -p $(CACERT_DIR)
	@echo "Fetching Mozilla CA bundle from curl.se …"
	curl $(CURL_RETRY) -fsSL https://curl.se/ca/cacert.pem -o $(CACERT_PEM)
	curl $(CURL_RETRY) -fsSL https://curl.se/ca/cacert.pem.sha256 -o $(CACERT_SHA256)
	@echo "Verifying SHA-256 …"
	@cd $(CACERT_DIR) && (sha256sum -c cacert.pem.sha256 2>/dev/null \
	    || shasum -a 256 -c cacert.pem.sha256)
	@echo "Done - $$(grep -c '^-----BEGIN CERTIFICATE-----' $(CACERT_PEM)) certificates."

# ── Embedded pwned-password blocklist (SecLists top 10K) ─────────────
#
# Refreshes stdlib/{lua,js}/hull/web/_pwned_blocklist.{lua,js} from
# the upstream SecLists 10K most-common passwords list. Output is a
# sorted, deduped list of 8-char uppercase SHA-1 prefixes, binary-
# searched at request time by hull/web/pwned BEFORE the network
# round-trip. Keeps the air-gapped fail-open from silently letting
# every weak password through. Source license: CC-BY-3.0.

PWNED_SRC_URL := https://raw.githubusercontent.com/danielmiessler/SecLists/master/Passwords/Common-Credentials/10k-most-common.txt
PWNED_SRC_TMP := /tmp/seclists_top10k.txt

.PHONY: fetch-pwned-blocklist
fetch-pwned-blocklist:
	@echo "Fetching SecLists top 10K from danielmiessler/SecLists …"
	curl $(CURL_RETRY) -fsSL $(PWNED_SRC_URL) -o $(PWNED_SRC_TMP)
	@bash scripts/build_pwned_blocklist.sh $(PWNED_SRC_TMP)

# ── HTMX + Pico (vendored assets for `hull init --profile htmx`) ───
#
# Pinned releases. Bumping versions: update the version + SHA-256
# variables below, run `make fetch-htmx fetch-pico`, sanity-check
# that the new bytes serve a working HTMX scaffold, then commit.
#
# These files are committed under vendor/htmx/ and vendor/pico/ at
# the pinned versions. They get embedded by the scaffold step (see
# §1.5.a-5 - `hull init --profile htmx`) when generating a new app's
# static/vendor/ directory. Apps own their copies after scaffolding;
# bumping Hull's pinned versions doesn't touch existing apps.

HTMX_DIR        := vendor/htmx
HTMX_VERSION    := v2.0.9
HTMX_MIN_SHA256 := 57d9191515339922bd1356d7b2d80b1ee3b29f1b3a2c65a078bb8b2e8fd9ae5f
HTMX_URL        := https://github.com/bigskysoftware/htmx/releases/download/$(HTMX_VERSION)/htmx.min.js
HTMX_MIN_JS     := $(HTMX_DIR)/htmx.min.js

PICO_DIR             := vendor/pico
PICO_VERSION         := v2.1.1
PICO_CLASSLESS_SHA256 := 61207a40ffc02a42d1e50143651c121beab70ed413c934c1ff84fa263ba436b0
PICO_URL             := https://raw.githubusercontent.com/picocss/pico/$(PICO_VERSION)/css/pico.classless.min.css
PICO_CLASSLESS_CSS   := $(PICO_DIR)/pico.classless.min.css

.PHONY: fetch-htmx
fetch-htmx:
	@mkdir -p $(HTMX_DIR)
	@echo "Fetching HTMX $(HTMX_VERSION) from github.com/bigskysoftware/htmx …"
	curl $(CURL_RETRY) -fsSL $(HTMX_URL) -o $(HTMX_MIN_JS)
	@echo "Verifying SHA-256 (pinned: $(HTMX_MIN_SHA256)) …"
	@actual=$$(shasum -a 256 $(HTMX_MIN_JS) | awk '{print $$1}'); \
	if [ "$$actual" != "$(HTMX_MIN_SHA256)" ]; then \
	    echo "SHA-256 mismatch for HTMX!"; \
	    echo "  expected: $(HTMX_MIN_SHA256)"; \
	    echo "  actual:   $$actual"; \
	    rm -f $(HTMX_MIN_JS); \
	    exit 1; \
	fi
	@echo "$(HTMX_VERSION)" > $(HTMX_DIR)/VERSION
	@echo "Done - htmx.min.js ($$(wc -c < $(HTMX_MIN_JS)) bytes)."

.PHONY: fetch-pico
fetch-pico:
	@mkdir -p $(PICO_DIR)
	@echo "Fetching Pico classless $(PICO_VERSION) from github.com/picocss/pico …"
	curl $(CURL_RETRY) -fsSL $(PICO_URL) -o $(PICO_CLASSLESS_CSS)
	@echo "Verifying SHA-256 (pinned: $(PICO_CLASSLESS_SHA256)) …"
	@actual=$$(shasum -a 256 $(PICO_CLASSLESS_CSS) | awk '{print $$1}'); \
	if [ "$$actual" != "$(PICO_CLASSLESS_SHA256)" ]; then \
	    echo "SHA-256 mismatch for Pico!"; \
	    echo "  expected: $(PICO_CLASSLESS_SHA256)"; \
	    echo "  actual:   $$actual"; \
	    rm -f $(PICO_CLASSLESS_CSS); \
	    exit 1; \
	fi
	@echo "$(PICO_VERSION)" > $(PICO_DIR)/VERSION
	@echo "Done - pico.classless.min.css ($$(wc -c < $(PICO_CLASSLESS_CSS)) bytes)."

.PHONY: fetch-htmx-pico
fetch-htmx-pico: fetch-htmx fetch-pico

# ── Unicode width data (refresh + regenerate) ──────────────────────
#
# `make fetch-unicode` downloads fresh EastAsianWidth.txt and
# UnicodeData.txt from unicode.org, verifies SHA-256 against the
# pinned checksums, then regenerates vendor/unicode/eaw.h via
# vendor/unicode/gen.lua. Checked-in artifacts cover hermetic builds;
# this target is invoked manually when upgrading Unicode versions.
#
# Pin to a specific Unicode release via HL_UNICODE_VERSION.

HL_UNICODE_VERSION ?= 16.0.0
UNICODE_BASE_URL   := https://www.unicode.org/Public/$(HL_UNICODE_VERSION)/ucd
UNICODE_EAW_TXT    := $(UNICODE_DIR)/EastAsianWidth.txt
UNICODE_UCD_TXT    := $(UNICODE_DIR)/UnicodeData.txt
UNICODE_EAW_H      := $(UNICODE_DIR)/eaw.h

.PHONY: fetch-unicode
fetch-unicode:
	@mkdir -p $(UNICODE_DIR)
	@echo "Fetching Unicode $(HL_UNICODE_VERSION) data from unicode.org …"
	curl $(CURL_RETRY) -fsSL $(UNICODE_BASE_URL)/EastAsianWidth.txt -o $(UNICODE_EAW_TXT)
	curl $(CURL_RETRY) -fsSL $(UNICODE_BASE_URL)/UnicodeData.txt   -o $(UNICODE_UCD_TXT)
	@echo "Recording SHA-256 …"
	@cd $(UNICODE_DIR) && (shasum -a 256 EastAsianWidth.txt > EastAsianWidth.txt.sha256 \
	    || sha256sum    EastAsianWidth.txt > EastAsianWidth.txt.sha256)
	@cd $(UNICODE_DIR) && (shasum -a 256 UnicodeData.txt   > UnicodeData.txt.sha256 \
	    || sha256sum    UnicodeData.txt   > UnicodeData.txt.sha256)
	@echo "Regenerating eaw.h via gen.lua …"
	@command -v lua >/dev/null && LUA=lua || LUA=luajit; \
	    $$LUA $(UNICODE_DIR)/gen.lua $(UNICODE_EAW_TXT) $(UNICODE_UCD_TXT) > $(UNICODE_EAW_H)
	@echo "Done - $$(grep -c '^    {' $(UNICODE_EAW_H)) ranges in $(UNICODE_EAW_H)."

# ── Vendored-library / toolchain fetchers ───────────────────────────

# The large binary downloads (wgpu-native, DuckDB static libs, the cosmocc

# toolchain), moved here from the root Makefile so every network-fetch target

# lives in one file. Self-contained: the WGPU_*/DUCKDB_*/COSMOCC_* config below

# is used only by these targets. References CURL_RETRY / VENDDIR / UNAME_* from

# the root (all defined before this file is included).
# ── Dependency fetching ──────────────────────────────────────────────

# wgpu-native v27.0.4.0 - GPU compute backend
WGPU_VERSION := v27.0.4.0
WGPU_SHA256_macos_aarch64 := 15367c26fdbe6892db35007d39f3883593384e777360b70e6bd704cb5dedde53
WGPU_SHA256_macos_x86_64  := 660fe9be59b555ec1d7c839e5cf8b6c71762938af61ab444a7a58dd87970dba2
WGPU_SHA256_linux_x86_64  := 271481ef76fbf3ea09631a6079e9493636ecf813cd9c92306c44a1a452991ba1
WGPU_SHA256_linux_aarch64  := a2f22248200997b69373273b10d50a58164f6ed840877289f3e46bff317b134e

# SHA-256 helper (portable: macOS uses shasum, Linux uses sha256sum)
SHA256CMD := $(shell command -v sha256sum 2>/dev/null || echo "$(SHA256CMD)")

# Detect platform for wgpu-native download
WGPU_OS := $(shell uname -s | tr A-Z a-z | sed 's/darwin/macos/')
WGPU_ARCH := $(shell uname -m | sed 's/arm64/aarch64/')
WGPU_PLATFORM := $(WGPU_OS)-$(WGPU_ARCH)
WGPU_ZIP := wgpu-$(WGPU_PLATFORM)-release.zip
WGPU_URL := https://github.com/gfx-rs/wgpu-native/releases/download/$(WGPU_VERSION)/$(WGPU_ZIP)
WGPU_EXPECTED_SHA := $(WGPU_SHA256_$(subst -,_,$(WGPU_PLATFORM)))

# DuckDB v1.5.4 - side-loaded static OLAP backend (make HL_ENABLE_DUCKDB=1).
# Prebuilt per-platform static-libs zip (headers + libduckdb_static.a + deps +
# default extensions). glibc Linux + macOS only; musl / windows / cosmo unsupported.
DUCKDB_VERSION := v1.5.4
DUCKDB_SHA256_osx_arm64    := 7d6d51110134c031e8a4944a8aa6514d68d462c479346a42f3e38bd7e4158c83
DUCKDB_SHA256_osx_amd64    := f102a62959e3cc7f2147c3181e6b86be158f70183889bb7b40624a4c18d886f3
DUCKDB_SHA256_linux_amd64  := 44edc1b55365624b4aa4a4f1d8087f75c4bfaceed4494b71059f54a8fa2f6e45
DUCKDB_SHA256_linux_arm64  := 68133154f3f62f5b8704656ece5b67989a3089537e4944e0b372518843155e62

# DuckDB uses osx/amd64/arm64 in its asset names (vs wgpu's macos/x86_64/aarch64).
DUCKDB_OS := $(shell uname -s | tr A-Z a-z | sed 's/darwin/osx/')
DUCKDB_ARCH := $(shell uname -m | sed 's/aarch64/arm64/;s/x86_64/amd64/')
DUCKDB_PLATFORM := $(DUCKDB_OS)-$(DUCKDB_ARCH)
DUCKDB_ZIP := static-libs-$(DUCKDB_PLATFORM).zip
DUCKDB_URL := https://github.com/duckdb/duckdb/releases/download/$(DUCKDB_VERSION)/$(DUCKDB_ZIP)
DUCKDB_EXPECTED_SHA := $(DUCKDB_SHA256_$(subst -,_,$(DUCKDB_PLATFORM)))

.PHONY: fetch-wgpu fetch-duckdb fetch-cosmocc

fetch-wgpu:
	@if [ -f $(VENDDIR)/wgpu/libwgpu_native.a ]; then \
		echo "wgpu-native already present at $(VENDDIR)/wgpu/"; \
	else \
		echo "=== Fetching wgpu-native $(WGPU_VERSION) for $(WGPU_PLATFORM) ==="; \
		if [ -z "$(WGPU_EXPECTED_SHA)" ]; then \
			echo "ERROR: unsupported platform $(WGPU_PLATFORM)"; \
			echo "Supported: macos-aarch64, macos-x86_64, linux-x86_64, linux-aarch64"; \
			exit 1; \
		fi; \
		curl $(CURL_RETRY_LARGE) -sL -o /tmp/$(WGPU_ZIP) "$(WGPU_URL)"; \
		echo "Verifying SHA-256..."; \
		ACTUAL=$$($(SHA256CMD) /tmp/$(WGPU_ZIP) | cut -d' ' -f1); \
		if [ "$$ACTUAL" != "$(WGPU_EXPECTED_SHA)" ]; then \
			echo "ERROR: SHA-256 mismatch!"; \
			echo "  expected: $(WGPU_EXPECTED_SHA)"; \
			echo "  actual:   $$ACTUAL"; \
			rm -f /tmp/$(WGPU_ZIP); \
			exit 1; \
		fi; \
		echo "SHA-256 OK"; \
		mkdir -p $(VENDDIR)/wgpu; \
		unzip -o -j /tmp/$(WGPU_ZIP) "lib/libwgpu_native.a" -d $(VENDDIR)/wgpu/; \
		unzip -o -j /tmp/$(WGPU_ZIP) "include/webgpu/webgpu.h" -d $(VENDDIR)/wgpu/; \
		unzip -o -j /tmp/$(WGPU_ZIP) "include/webgpu/wgpu.h" -d $(VENDDIR)/wgpu/; \
		rm -f /tmp/$(WGPU_ZIP); \
		echo "=== wgpu-native $(WGPU_VERSION) installed to $(VENDDIR)/wgpu/ ==="; \
		ls -lh $(VENDDIR)/wgpu/libwgpu_native.a; \
	fi

fetch-duckdb:
	@if [ -f $(VENDDIR)/duckdb/libduckdb_static.a ]; then \
		echo "DuckDB static libs already present at $(VENDDIR)/duckdb/"; \
	else \
		echo "=== Fetching DuckDB $(DUCKDB_VERSION) for $(DUCKDB_PLATFORM) ==="; \
		if [ -z "$(DUCKDB_EXPECTED_SHA)" ]; then \
			echo "ERROR: unsupported platform $(DUCKDB_PLATFORM)"; \
			echo "Supported: osx-arm64, osx-amd64, linux-amd64, linux-arm64 (glibc)"; \
			exit 1; \
		fi; \
		curl $(CURL_RETRY_LARGE) -sL -o /tmp/$(DUCKDB_ZIP) "$(DUCKDB_URL)"; \
		echo "Verifying SHA-256..."; \
		ACTUAL=$$($(SHA256CMD) /tmp/$(DUCKDB_ZIP) | cut -d' ' -f1); \
		if [ "$$ACTUAL" != "$(DUCKDB_EXPECTED_SHA)" ]; then \
			echo "ERROR: SHA-256 mismatch!"; \
			echo "  expected: $(DUCKDB_EXPECTED_SHA)"; \
			echo "  actual:   $$ACTUAL"; \
			rm -f /tmp/$(DUCKDB_ZIP); \
			exit 1; \
		fi; \
		echo "SHA-256 OK"; \
		mkdir -p $(VENDDIR)/duckdb; \
		unzip -o -j /tmp/$(DUCKDB_ZIP) -d $(VENDDIR)/duckdb/; \
		rm -f /tmp/$(DUCKDB_ZIP); \
		echo "Isolating DuckDB's bundled mbedTLS/psa symbols..."; \
		OBJCOPY="$$(command -v objcopy || command -v llvm-objcopy || command -v gobjcopy || true)"; \
		[ -z "$$OBJCOPY" ] && [ -x /opt/homebrew/opt/llvm/bin/llvm-objcopy ] && OBJCOPY=/opt/homebrew/opt/llvm/bin/llvm-objcopy; \
		[ -z "$$OBJCOPY" ] && [ -x /usr/local/opt/llvm/bin/llvm-objcopy ] && OBJCOPY=/usr/local/opt/llvm/bin/llvm-objcopy; \
		if [ -z "$$OBJCOPY" ]; then \
			echo "ERROR: fetch-duckdb needs objcopy or llvm-objcopy to isolate DuckDB's bundled"; \
			echo "mbedTLS (a different version than Hull's; they would collide at link)."; \
			echo "Install: 'apt-get install binutils' (Linux) or 'brew install llvm' (macOS)."; \
			exit 1; \
		fi; \
		LIB=$(VENDDIR)/duckdb/libduckdb_static.a; \
		nm "$$LIB" 2>/dev/null | awk '{print $$NF}' | grep -E '^_?(mbedtls_|psa_)' | sort -u \
			| awk '{print $$1" hlduck_"$$1}' > /tmp/hl_duckdb_syms.map; \
		if [ -s /tmp/hl_duckdb_syms.map ]; then \
			"$$OBJCOPY" --redefine-syms=/tmp/hl_duckdb_syms.map "$$LIB"; \
			echo "isolated $$(wc -l < /tmp/hl_duckdb_syms.map | tr -d ' ') symbols (hlduck_ prefix)"; \
		fi; \
		rm -f /tmp/hl_duckdb_syms.map; \
		echo "=== DuckDB $(DUCKDB_VERSION) installed to $(VENDDIR)/duckdb/ ==="; \
		ls -lh $(VENDDIR)/duckdb/libduckdb_static.a; \
	fi

# Cosmopolitan cosmocc 4.0.2 - portable C compiler
COSMOCC_VERSION := 4.0.2
COSMOCC_SHA256 := 85b8c37a406d862e656ad4ec14be9f6ce474c1b436b9615e91a55208aced3f44
COSMOCC_URL := https://cosmo.zip/pub/cosmocc/cosmocc-$(COSMOCC_VERSION).zip
COSMOCC_DIR ?= /opt/cosmo

fetch-cosmocc:
	@if command -v cosmocc >/dev/null 2>&1; then \
		echo "cosmocc already installed: $$(which cosmocc)"; \
	else \
		echo "=== Fetching cosmocc $(COSMOCC_VERSION) to $(COSMOCC_DIR) ==="; \
		curl $(CURL_RETRY_LARGE) -sL -o /tmp/cosmocc.zip "$(COSMOCC_URL)"; \
		echo "Verifying SHA-256..."; \
		ACTUAL=$$($(SHA256CMD) /tmp/cosmocc.zip | cut -d' ' -f1); \
		if [ "$$ACTUAL" != "$(COSMOCC_SHA256)" ]; then \
			echo "ERROR: SHA-256 mismatch!"; \
			echo "  expected: $(COSMOCC_SHA256)"; \
			echo "  actual:   $$ACTUAL"; \
			rm -f /tmp/cosmocc.zip; \
			exit 1; \
		fi; \
		echo "SHA-256 OK"; \
		mkdir -p $(COSMOCC_DIR); \
		unzip -q -o /tmp/cosmocc.zip -d $(COSMOCC_DIR); \
		rm -f /tmp/cosmocc.zip; \
		echo "=== cosmocc $(COSMOCC_VERSION) installed to $(COSMOCC_DIR)/bin/cosmocc ==="; \
		echo "Add to PATH: export PATH=$(COSMOCC_DIR)/bin:\$$PATH"; \
	fi
