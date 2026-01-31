#!/bin/sh

BUILD_DIR="cmake-build-debug"

# This script stays running and keeps QEMU alive
trap 'killall qemu-system-x86_64 2>/dev/null; exit' INT TERM EXIT

qemu-system-x86_64 \
    -kernel "$BUILD_DIR/kernel/Kernel" \
    -no-reboot \
    -nographic \
    -s -S \
    -smp 4 \
    -m 256M \
    -d guest_errors \
    -cpu qemu64,+fsgsbase \
    -serial file:"$BUILD_DIR/qemu.log"