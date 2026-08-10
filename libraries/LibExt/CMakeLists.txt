# ─── LibExt: header-only, on purpose ───────────────────────────────────────
#
# EXT2 on-disk structures and nothing else — `Ext2Superblock` and its
# `static_assert(sizeof(...) == 264)`, which is the whole point of the header: the
# layout is checked at compile time by every TU that includes it. There is no code
# to compile, so this is an INTERFACE library. `docs`/CLAUDE.md's library table has
# recorded LibExt's target as "-" all along; this makes the build agree.
#
# ─── What was here, and why it broke the default `all` target (R-17) ────────
#
# A STATIC library over `Temp.cpp` and an executable `TestDriver` over
# `TestDriver.cpp`, plus a block that reassigned `CMAKE_C_COMPILER` /
# `CMAKE_CXX_COMPILER` to Homebrew clang inside this subdirectory.
#
# Three separate problems, and the middle one is the instructive one:
#
#   * **The source files did not exist on `master`** — the two `set()` calls named
#     files that were never committed, so configuring the tree failed outright.
#     Two 15-line stubs (an empty TU and a `main()` printing "AAAA") then arrived
#     as stray additions in an unrelated radix commit, which turned the configure
#     error into a compile error rather than fixing anything.
#   * **The compiler override could not work and made things worse.** CMake fixes
#     the compiler at `project()` time, so reassigning it in a subdirectory does
#     not re-detect a toolchain — it just swaps the binary while every flag in
#     `CMAKE_CXX_FLAGS` is still the cross-GCC set the top level chose. Host clang
#     then met GCC-only `-Wno-comma-subscript` under `-Werror` and refused, which
#     is the error the default target actually reported. It never got as far as the
#     `<iostream>` in a `-nostdlib -ffreestanding` TU that would have failed next.
#   * **A host-compiled target does not belong in this tree.** The kernel tree is
#     cross-compiled end to end; everything host-compiled lives in `tests/`, which
#     is its own CMake project for exactly this reason. That separation is the
#     project's existing answer and the reason the override was reached for.
#
# So the driver is gone rather than repaired. If EXT2 grows an implementation it
# gets sources here (cross-compiled, like every other library) and its tests go
# under `tests/`, where a `main()` and a standard library are legal.

add_library(Ext INTERFACE)

target_include_directories(Ext INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../Core/include
)
