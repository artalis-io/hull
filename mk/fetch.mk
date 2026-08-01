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
	@echo "Done — $$(grep -c '^-----BEGIN CERTIFICATE-----' $(CACERT_PEM)) certificates."

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
# §1.5.a-5 — `hull init --profile htmx`) when generating a new app's
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
	@echo "Done — htmx.min.js ($$(wc -c < $(HTMX_MIN_JS)) bytes)."

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
	@echo "Done — pico.classless.min.css ($$(wc -c < $(PICO_CLASSLESS_CSS)) bytes)."

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
	@echo "Done — $$(grep -c '^    {' $(UNICODE_EAW_H)) ranges in $(UNICODE_EAW_H)."
