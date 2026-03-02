#!/usr/bin/env bash
#
# Start the todo app in development mode (hot reload + HTTPS).
#
# Usage: ./examples/todo/dev.sh
#
# Requires: hull binary built via `make`
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
HULL="$REPO_ROOT/build/hull"

if [ ! -x "$HULL" ]; then
    echo "hull binary not found at $HULL"
    echo "Run 'make' from the repo root first."
    exit 1
fi

# Generate self-signed certs if needed
"$SCRIPT_DIR/gen-certs.sh"

PORT="${PORT:-8443}"
DB="${DB:-/tmp/todo.db}"
CERT="$SCRIPT_DIR/certs/cert.pem"
KEY="$SCRIPT_DIR/certs/key.pem"

echo "Starting todo app in dev mode (HTTPS)..."
echo "  https://localhost:$PORT"
echo "  Database: $DB"
echo ""

exec "$HULL" dev "$SCRIPT_DIR/app.lua" -p "$PORT" -d "$DB" \
    --tls-cert "$CERT" --tls-key "$KEY"
