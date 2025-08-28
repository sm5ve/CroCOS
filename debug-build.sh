#!/bin/sh

#hacky little script because I was having trouble getting clion's remote debugging
#to automatically build and run the kernel

set -euo pipefail

killall qemu-system-x86_64 || true

BUILD_DIR="cmake-build-amd64-kernel-debug"

cmake --build "$BUILD_DIR" --target Kernel

qemu-system-x86_64 -kernel $BUILD_DIR/kernel/Kernel -no-reboot -nographic -s -S \
-smp 4 -m 256M -d guest_errors -cpu qemu64,+fsgsbase -serial file:$BUILD_DIR/qemu.log &

sleep 1