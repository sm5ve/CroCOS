#!/bin/sh

set -euo pipefail

BUILD_DIR="cmake-build-debug"

killall qemu-system-x86_64 2>/dev/null || true
sleep 0.5

echo "Building kernel..."
cmake --build "$BUILD_DIR" --target Kernel

echo "Starting QEMU wrapper..."

# Start the wrapper in the background using macOS-specific launchd-style detachment
nohup ./qemu_wrapper.sh > /tmp/qemu_wrapper.log 2>&1 &

# Wait for QEMU to be ready
for i in $(seq 1 20); do
    if lsof -i :1234 >/dev/null 2>&1; then
        echo "QEMU ready on port 1234"
        exit 0
    fi
    sleep 0.3
done

echo "ERROR: Timeout waiting for QEMU"
cat /tmp/qemu_wrapper.log
exit 1