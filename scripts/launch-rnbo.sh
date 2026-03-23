#!/bin/bash
set -e
RNBO_DIR="/data/UserData/rnbo"
export LD_LIBRARY_PATH="$RNBO_DIR/lib:$LD_LIBRARY_PATH"

echo "Starting JACK with shadow driver..."
"$RNBO_DIR/bin/jackd" -d shadow &
JACK_PID=$!
sleep 1
if ! kill -0 $JACK_PID 2>/dev/null; then
    echo "ERROR: jackd failed to start"; exit 1
fi

echo "Starting RNBO runner..."
"$RNBO_DIR/bin/rnbooscquery" --wait-for-audio --delay-start &
echo "Starting Link transport bridge..."
"$RNBO_DIR/bin/jack_transport_link" &
echo "Starting RNBO web panel..."
"$RNBO_DIR/bin/rnbo-runner-panel" \
    --static-dir "$RNBO_DIR/share/rnbo-runner-panel/www/" \
    --template-dir "$RNBO_DIR/share/rnbo-runner-panel/templates/" &

echo "RNBO stack running. Web panel at http://move.local:5678"
trap 'echo "Stopping..."; kill $(jobs -p) 2>/dev/null; wait' INT TERM
wait
