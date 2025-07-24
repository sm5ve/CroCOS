#!/bin/bash
# Script to instrument object files for memory tracking
# Created by Spencer Martin on 7/24/25.

BUILD_DIR="$1"

# Find llvm-objcopy - check PATH first, then fallback to Homebrew location
if command -v llvm-objcopy >/dev/null 2>&1; then
    OBJCOPY="llvm-objcopy"
elif [ -x "/opt/homebrew/opt/llvm/bin/llvm-objcopy" ]; then
    OBJCOPY="/opt/homebrew/opt/llvm/bin/llvm-objcopy"
else
    echo "Error: llvm-objcopy not found in PATH or /opt/homebrew/opt/llvm/bin/"
    exit 1
fi

echo "Using objcopy: $OBJCOPY"
echo "Instrumenting object files in: $BUILD_DIR"

# Find all object files and instrument them
find "$BUILD_DIR/CMakeFiles/CoreLibraryTests.dir" -name "*.o" | while read -r obj_file; do
    echo "Processing: $(basename "$obj_file")"
    
    # First pass: rename allocation symbols
    $OBJCOPY \
        --redefine-sym malloc=__tracked_malloc \
        --redefine-sym free=__tracked_free \
        --redefine-sym calloc=__tracked_calloc \
        --redefine-sym realloc=__tracked_realloc \
        --redefine-sym _malloc=__tracked_malloc \
        --redefine-sym _free=__tracked_free \
        --redefine-sym _calloc=__tracked_calloc \
        --redefine-sym _realloc=__tracked_realloc \
        --redefine-sym _Znwm=__tracked_new \
        --redefine-sym _Znam=__tracked_new_array \
        --redefine-sym _ZdlPv=__tracked_delete \
        --redefine-sym _ZdaPv=__tracked_delete_array \
        --redefine-sym _ZdlPvm=__tracked_delete_sized \
        --redefine-sym _ZdaPvm=__tracked_delete_array_sized \
        --redefine-sym __Znwm=__tracked_new \
        --redefine-sym __Znam=__tracked_new_array \
        --redefine-sym __ZdlPv=__tracked_delete \
        --redefine-sym __ZdaPv=__tracked_delete_array \
        --redefine-sym __ZdlPvm=__tracked_delete_sized \
        --redefine-sym __ZdaPvm=__tracked_delete_array_sized \
        "$obj_file" "$obj_file.tmp"
    
    # Second pass: weaken the renamed symbols
    $OBJCOPY \
        --weaken-symbol __tracked_malloc \
        --weaken-symbol __tracked_free \
        --weaken-symbol __tracked_calloc \
        --weaken-symbol __tracked_realloc \
        --weaken-symbol __tracked_new \
        --weaken-symbol __tracked_new_array \
        --weaken-symbol __tracked_delete \
        --weaken-symbol __tracked_delete_array \
        --weaken-symbol __tracked_delete_sized \
        --weaken-symbol __tracked_delete_array_sized \
        "$obj_file.tmp" "$obj_file.final"
    
    # Replace original file
    mv "$obj_file.final" "$obj_file"
    rm -f "$obj_file.tmp"
done

echo "Object file instrumentation complete"