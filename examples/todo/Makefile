# examples/todo/Makefile — Build and run the todo example app
#
# Usage:
#   make dev                    # dev server with hot reload + HTTPS (Lua)
#   make dev ENTRY=app.js       # dev server with JS backend
#   make prod                   # build native production binary
#   make prod CC=cosmocc        # build portable APE binary
#   make run                    # run the production binary
#   make test                   # run app tests
#   make certs                  # generate self-signed TLS certs
#   make clean                  # remove production binary

REPO_ROOT := $(abspath ../..)
HULL      := $(REPO_ROOT)/build/hull
PORT      := 8443
DB        := /tmp/todo.db
ENTRY     := app.lua
CC        := cc
JOBS      := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: dev prod run test certs clean

# ── Dev server (hot reload + HTTPS) ─────────────────────────────────

dev: certs
	@test -x "$(HULL)" || $(MAKE) -j$(JOBS) -C $(REPO_ROOT)
	$(HULL) dev $(ENTRY) -p $(PORT) -d $(DB) \
		--tls-cert certs/cert.pem --tls-key certs/key.pem

# ── Production build ────────────────────────────────────────────────
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
	$(MAKE) -j$(JOBS) -C $(REPO_ROOT) $(EMBED_FLAG)
	$(HULL) build --cc $(CC) -o todo .

# ── Run production binary ───────────────────────────────────────────

run: certs
	@test -x todo || { echo "Build first: make prod"; exit 1; }
	./todo -p $(PORT) -d $(DB) \
		--tls-cert certs/cert.pem --tls-key certs/key.pem \
		$(ENTRY)

# ── Tests ───────────────────────────────────────────────────────────

test:
	@test -x "$(HULL)" || $(MAKE) -j$(JOBS) -C $(REPO_ROOT)
	$(HULL) test .

# ── Self-signed TLS certificates ────────────────────────────────────

certs: certs/cert.pem

certs/cert.pem:
	@mkdir -p certs
	openssl ecparam -genkey -name prime256v1 -noout -out certs/key.pem
	openssl req -new -x509 -key certs/key.pem -out certs/cert.pem -days 365 \
		-subj "/CN=localhost" \
		-addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

# ── Clean ───────────────────────────────────────────────────────────

clean:
	rm -f todo
