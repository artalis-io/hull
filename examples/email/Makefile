# examples/email/Makefile — Build and run the email example app
#
# Usage:
#   make dev                    # dev server (Lua)
#   make dev ENTRY=app.js       # dev server with JS backend
#   make prod                   # build native production binary
#   make prod CC=cosmocc        # build portable APE binary
#   make run                    # run the production binary
#   make test                   # run app tests
#   make clean                  # remove build artifacts

REPO_ROOT := $(abspath ../..)
HULL      := $(REPO_ROOT)/build/hull
PORT      := 3000
DB        := /tmp/email.db
ENTRY     := app.lua
CC        := cc
JOBS      := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: dev prod run test mock clean

# ── Dev server ─────────────────────────────────────────────────────

dev:
	@test -x "$(HULL)" || $(MAKE) -j$(JOBS) -C $(REPO_ROOT)
	$(HULL) dev $(ENTRY) -p $(PORT) -d $(DB)

# ── Production build ──────────────────────────────────────────────
#
# hull build requires the platform library embedded inside the hull binary.
# For cosmo:  make platform-cosmo && make EMBED_PLATFORM=cosmo
# For native: make platform && make EMBED_PLATFORM=1
#
# IMPORTANT: The hull binary is a host tool — always built with the native
# compiler. CC is only forwarded to `hull build` for the app binary.

ifneq (,$(findstring cosmo,$(CC)))
  PLATFORM_TARGET := platform-cosmo
  EMBED_FLAG      := EMBED_PLATFORM=cosmo
else
  PLATFORM_TARGET := platform
  EMBED_FLAG      := EMBED_PLATFORM=1
endif

prod:
	$(MAKE) -j$(JOBS) -C $(REPO_ROOT) $(PLATFORM_TARGET)
	$(MAKE) -j$(JOBS) -C $(REPO_ROOT) $(EMBED_FLAG) CC=cc AR=ar
	$(HULL) build --cc $(CC) -o email .

# ── Run production binary ─────────────────────────────────────────

run:
	@test -x email || { echo "Build first: make prod"; exit 1; }
	./email -p $(PORT) -d $(DB) $(ENTRY)

# ── Tests ─────────────────────────────────────────────────────────

test:
	@test -x "$(HULL)" || $(MAKE) -j$(JOBS) -C $(REPO_ROOT)
	$(HULL) test .

# ── Mock SMTP server (for local testing) ──────────────────────────

mock:
	$(CC) -Wall -Wextra -o /tmp/mock_smtp mock_smtp.c
	@echo "Starting mock SMTP server..."
	@rm -f /tmp/smtp_port /tmp/smtp_data
	/tmp/mock_smtp /tmp/smtp_port /tmp/smtp_data &
	@sleep 0.5
	@echo "Mock SMTP listening on port $$(cat /tmp/smtp_port)"
	@echo "Run: SMTP_HOST=127.0.0.1 SMTP_PORT=$$(cat /tmp/smtp_port) SMTP_TLS=false make dev"

# ── Clean ─────────────────────────────────────────────────────────

clean:
	rm -f email /tmp/mock_smtp
