#!/usr/bin/env bash
# Install jack_shadow.so to Move
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MOVE_HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/rnbo/lib/jack"
ARTIFACT="$REPO_ROOT/build/jack_shadow.so"

if [[ ! -f "$ARTIFACT" ]]; then
    echo "Error: build/jack_shadow.so not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "==> Installing jack_shadow.so to $MOVE_HOST..."

ssh -o ConnectTimeout=10 "ableton@${MOVE_HOST}" "mkdir -p ${DEST}"
scp -o ConnectTimeout=10 "$ARTIFACT" "ableton@${MOVE_HOST}:${DEST}/jack_shadow.so"

echo "==> Done. Start JACK with: jackd -d shadow"
