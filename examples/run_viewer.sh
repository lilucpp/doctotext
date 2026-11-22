#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${PORT:-8765}"
HOST="${HOST:-127.0.0.1}"

exec python3 -B "${SCRIPT_DIR}/viewer/server.py" --host "${HOST}" --port "${PORT}" "$@"
