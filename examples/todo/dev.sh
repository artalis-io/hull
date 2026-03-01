#!/usr/bin/env bash
#
# Start the todo app in development mode (hot reload).
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

PORT="${PORT:-8080}"
DB="${DB:-/tmp/todo.db}"

echo "Starting todo app in dev mode..."
echo "  http://localhost:$PORT"
echo "  Database: $DB"
echo ""

exec "$HULL" dev "$SCRIPT_DIR/app.lua" -p "$PORT" -d "$DB"
