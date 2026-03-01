#!/usr/bin/env bash
#
# Build the todo app into a standalone binary, then (optionally) run it.
#
# Usage:
#   ./examples/todo/build.sh          # build only
#   ./examples/todo/build.sh --run    # build and run
#
# Requires: hull binary + platform library built via `make && make platform`
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
HULL="$REPO_ROOT/build/hull"
OUTPUT="$SCRIPT_DIR/todo"

if [ ! -x "$HULL" ]; then
    echo "hull binary not found at $HULL"
    echo "Run 'make' from the repo root first."
    exit 1
fi

echo "Building todo app..."
"$HULL" build -o "$OUTPUT" "$SCRIPT_DIR"

echo ""
echo "Built: $OUTPUT"
ls -lh "$OUTPUT"

if [ "${1:-}" = "--run" ]; then
    PORT="${PORT:-8080}"
    DB="${DB:-/tmp/todo.db}"
    echo ""
    echo "Running todo app..."
    echo "  http://localhost:$PORT"
    echo "  Database: $DB"
    echo ""
    exec "$OUTPUT" -p "$PORT" -d "$DB"
fi
