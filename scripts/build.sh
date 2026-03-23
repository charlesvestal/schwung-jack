#!/usr/bin/env bash
# Build jack_shadow.so for Move (ARM64)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-jack-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== JACK Shadow Driver Build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    echo "=== Done ==="
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

echo "=== Building jack_shadow.so ==="

# Include paths into the JACK source tree
JACK_INCLUDES="-I. -Icommon -Icommon/jack -Ilinux -Ilinux/shadow -Iposix"

# Defines: SERVER_SIDE for symbol export, skip config.h
DEFINES="-DSERVER_SIDE"

mkdir -p build

echo "Compiling JackShadowDriver..."
${CROSS_PREFIX}g++ -g -O2 -fPIC -std=c++17 \
    $DEFINES \
    $JACK_INCLUDES \
    -c linux/shadow/JackShadowDriver.cpp \
    -o build/JackShadowDriver.o

echo "Linking jack_shadow.so..."
${CROSS_PREFIX}g++ -shared \
    build/JackShadowDriver.o \
    -o build/jack_shadow.so \
    -lrt -lpthread

echo ""
echo "=== Build Complete ==="
echo "Output: build/jack_shadow.so"
echo ""
echo "Deploy: scp build/jack_shadow.so ableton@move.local:/data/UserData/rnbo/lib/jack/"
