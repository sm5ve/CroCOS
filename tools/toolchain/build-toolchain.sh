#!/usr/bin/env bash
#
# Build the x86_64-crocos cross toolchain.
#
# CroCOS cannot be built with a stock x86_64-elf cross compiler. See README.md
# in this directory for why; the short version is that a stock cross GCC emits
# global constructors into legacy .ctors sections, which the kernel's linker
# script does not collect, so they silently never run.
#
# Usage:
#   ./build-toolchain.sh              # build and install everything
#   JOBS=4 ./build-toolchain.sh       # limit parallelism
#
# Environment:
#   CROCOS_TOOLCHAIN_PREFIX   install location (default ~/opt/crocos-toolchain)
#   CROCOS_TOOLCHAIN_WORKDIR  scratch space  (default ~/.cache/crocos-toolchain)
#   JOBS                      make -j value  (default: CPU count)
#   KEEP_BUILD_DIRS=1         do not delete build trees after install
#
set -euo pipefail

BINUTILS_VERSION=2.44
GCC_VERSION=15.1.0
BINUTILS_SHA256=ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237
GCC_SHA256=e2b09ec21660f01fecffb715e0120265216943f038d0e48a9868713e54f06cea

TARGET=x86_64-crocos
PREFIX="${CROCOS_TOOLCHAIN_PREFIX:-$HOME/opt/crocos-toolchain}"
WORKDIR="${CROCOS_TOOLCHAIN_WORKDIR:-$HOME/.cache/crocos-toolchain}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_DIR="$SCRIPT_DIR/patches"

if [ -z "${JOBS:-}" ]; then
    JOBS="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu || echo 4)"
fi

SRC="$WORKDIR/src"
BUILD="$WORKDIR/build"

log()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------- environment hygiene --
# A toolchain build must not inherit ambient include/library search paths. These
# variables inject directories into the *system* search list, which sits below
# the -I flags the build passes explicitly -- except that the compiler then
# deduplicates any -I that matches one of them and keeps only the system-list
# copy, silently demoting it.
#
# The concrete failure this prevents: a trailing or leading ':' in
# C_INCLUDE_PATH is an empty entry meaning "the current directory", exactly as
# in PATH. That injects '.' into the system list, gas's explicit -I. is
# deduplicated against it and demoted below -I../bfd, and gas/as.c ends up
# including bfd's config.h instead of its own -- failing with a wall of
# "use of undeclared identifier 'TARGET_ALIAS'" that points nowhere near the
# real cause. GMP/MPFR/MPC are located via --with-* below, so nothing here is
# needed for a correct build.
for var in CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH \
           OBJCPLUS_INCLUDE_PATH INCLUDE LIBRARY_PATH LD_LIBRARY_PATH; do
    if [ -n "${!var:-}" ]; then
        printf '\033[1;33mnote:\033[0m ignoring %s for this build (was: %s)\n' "$var" "${!var}"
        unset "$var"
    fi
done

# ---------------------------------------------------------------- preflight --
for tool in curl tar make patch shasum; do
    command -v "$tool" >/dev/null || die "required tool not found: $tool"
done
command -v gcc >/dev/null || command -v clang >/dev/null || die "no host compiler"

# GCC needs GMP/MPFR/MPC. On macOS these come from Homebrew and must be pointed
# at explicitly; on Linux the distro packages are usually found automatically.
GCC_DEP_ARGS=()
if [ "$(uname -s)" = "Darwin" ]; then
    command -v brew >/dev/null || die "Homebrew is required on macOS (for gmp/mpfr/libmpc)"
    for dep in gmp mpfr libmpc; do
        brew --prefix "$dep" >/dev/null 2>&1 || die "missing dependency: brew install $dep"
    done
    GCC_DEP_ARGS=(
        "--with-gmp=$(brew --prefix gmp)"
        "--with-mpfr=$(brew --prefix mpfr)"
        "--with-mpc=$(brew --prefix libmpc)"
    )
fi

mkdir -p "$SRC" "$BUILD" "$PREFIX"

# ----------------------------------------------------------------- fetch -----
fetch() {
    local url="$1" file="$2" want="$3"
    if [ -f "$SRC/$file" ] && [ "$(shasum -a 256 "$SRC/$file" | cut -d' ' -f1)" = "$want" ]; then
        log "$file already present and verified"
        return
    fi
    log "downloading $file"
    curl -fL --progress-bar -o "$SRC/$file" "$url"
    local got
    got="$(shasum -a 256 "$SRC/$file" | cut -d' ' -f1)"
    [ "$got" = "$want" ] || die "checksum mismatch for $file: got $got, expected $want"
}

fetch "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
      "binutils-$BINUTILS_VERSION.tar.xz" "$BINUTILS_SHA256"
fetch "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
      "gcc-$GCC_VERSION.tar.xz" "$GCC_SHA256"

# --------------------------------------------------------- extract + patch ---
# Always start from a pristine tree so a half-applied patch from an interrupted
# run cannot silently produce a subtly wrong compiler.
prepare() {
    local name="$1" patchfile="$2"
    log "extracting $name"
    rm -rf "${SRC:?}/$name"
    tar -C "$SRC" -xf "$SRC/$name.tar.xz"
    log "patching $name for $TARGET"
    ( cd "$SRC/$name" && patch -p1 --forward < "$patchfile" )
    # binutils' ld/Makefile.in and gcc/configure are generated files that we
    # patch in lockstep with their sources. Neither project enables maintainer
    # mode by default so they will not be regenerated, but make the timestamps
    # unambiguous anyway.
    find "$SRC/$name" -name 'Makefile.in' -newer "$SRC/$name/configure" -exec touch {} + 2>/dev/null || true
}

prepare "binutils-$BINUTILS_VERSION" "$PATCH_DIR/binutils-$BINUTILS_VERSION-crocos.patch"
prepare "gcc-$GCC_VERSION"           "$PATCH_DIR/gcc-$GCC_VERSION-crocos.patch"

# ---------------------------------------------------------------- binutils ---
log "configuring binutils"
rm -rf "$BUILD/binutils"
mkdir -p "$BUILD/binutils"
(
    cd "$BUILD/binutils"
    # --with-system-zlib is required on macOS and harmless elsewhere. The zlib
    # bundled with binutils and GCC predates Mac OS X: it sees Apple's
    # TARGET_OS_MAC and concludes it is targeting *classic* Mac OS, which has no
    # fdopen(), so zutil.h does `#define fdopen(fd,mode) NULL` and the system
    # <stdio.h> fails to parse on the next include.
    "$SRC/binutils-$BINUTILS_VERSION/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot \
        --with-system-zlib \
        --disable-nls \
        --disable-werror \
        --enable-initfini-array
    log "building binutils (-j$JOBS)"
    make -j"$JOBS"
    make install
)

# The rest of the build needs the assembler and linker we just installed.
export PATH="$PREFIX/bin:$PATH"
command -v "$TARGET-as" >/dev/null || die "$TARGET-as not on PATH after binutils install"

# --------------------------------------------------------------------- gcc ---
# --enable-initfini-array is the point of this whole exercise. The patched
# acinclude.m4/configure already default it to yes for *-*-crocos*, but pass it
# explicitly too: it is cheap, and it means the build is still correct if the
# target-default hunk is ever dropped when rebasing the patch onto a new GCC.
log "configuring gcc"
rm -rf "$BUILD/gcc"
mkdir -p "$BUILD/gcc"
(
    cd "$BUILD/gcc"
    "$SRC/gcc-$GCC_VERSION/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --disable-nls \
        --enable-languages=c,c++ \
        --without-headers \
        --with-system-zlib \
        --enable-initfini-array \
        --disable-hosted-libstdcxx \
        "${GCC_DEP_ARGS[@]}"
    log "building gcc (-j$JOBS)"
    make -j"$JOBS" all-gcc
    log "building libgcc, all multilibs (-j$JOBS)"
    make -j"$JOBS" all-target-libgcc
    make install-gcc
    make install-target-libgcc
)

# ------------------------------------------------------------------ verify ---
log "verifying toolchain"
cat > "$BUILD/ctorcheck.cpp" <<'EOF'
struct S { int x; S(); };
extern int sink;
S::S() { sink = 1; }
static S global;
EOF

"$TARGET-g++" -c "$BUILD/ctorcheck.cpp" -o "$BUILD/ctorcheck.o" \
    -ffreestanding -fno-rtti -fno-exceptions -nostdlib \
    -mno-red-zone -mcmodel=kernel -O0

if "$TARGET-objdump" -h "$BUILD/ctorcheck.o" | grep -q '\.init_array'; then
    log "PASS: global constructors are emitted into .init_array"
elif "$TARGET-objdump" -h "$BUILD/ctorcheck.o" | grep -q '\.ctors'; then
    die "constructors still going to .ctors -- the initfini-array patch did not take effect"
else
    die "could not find a constructor section in the test object"
fi

# The target must ship a freestanding <stdint.h>. GCC only installs one when
# config.gcc sets use_gcc_stdint; with the default of "none" the target silently
# ships no stdint.h at all and every freestanding translation unit fails.
cat > "$BUILD/stdintcheck.c" <<'EOF'
#include <stdint.h>
uint64_t f(uint32_t x) { return (uint64_t) x + UINT64_C(1); }
EOF
if "$TARGET-gcc" -ffreestanding -nostdlib -c "$BUILD/stdintcheck.c" -o "$BUILD/stdintcheck.o" 2>/dev/null; then
    log "PASS: freestanding <stdint.h> is provided"
else
    die "the target provides no usable <stdint.h> (check use_gcc_stdint in gcc/config.gcc)"
fi

# crtstuff must exist for every multilib. The OS-level *-*-elf) case in
# libgcc/config.host has no trailing '*', so a new OS falls through it and gets
# no crtbegin.o/crtend.o unless it is added explicitly.
for mldir in "" "kernel/no-red-zone"; do
    mlflags=""
    [ -n "$mldir" ] && mlflags="-mcmodel=kernel -mno-red-zone"
    for part in crtbegin.o crtend.o crti.o crtn.o; do
        # shellcheck disable=SC2086
        partpath="$("$TARGET-gcc" $mlflags -print-file-name=$part)"
        [ -f "$partpath" ] \
            || die "$part missing for multilib '${mldir:-.}' (got '$partpath')"
    done
done
log "PASS: crtbegin/crtend/crti/crtn present for every multilib"

log "multilib configurations:"
"$TARGET-gcc" -print-multi-lib | sed 's/^/    /'

# Both of these must hold. If the kernel silently links the default
# -mcmodel=small libgcc, it works right up until it doesn't; and if the default
# multilib were pruned away, future userspace would have no libgcc at all.
KERNEL_LIBGCC="$("$TARGET-gcc" -mcmodel=kernel -mno-red-zone -print-libgcc-file-name)"
DEFAULT_LIBGCC="$("$TARGET-gcc" -print-libgcc-file-name)"
log "libgcc for kernel flags: $KERNEL_LIBGCC"
log "libgcc for default flags: $DEFAULT_LIBGCC"
case "$KERNEL_LIBGCC" in
    *kernel*) log "PASS: kernel flags select a dedicated libgcc multilib" ;;
    *) die "kernel flags resolve to the default libgcc ($KERNEL_LIBGCC); the multilib in gcc/config/i386/t-x86_64-crocos is not taking effect" ;;
esac
[ "$KERNEL_LIBGCC" != "$DEFAULT_LIBGCC" ] \
    || die "kernel and default libgcc are the same file ($KERNEL_LIBGCC)"
[ -f "$DEFAULT_LIBGCC" ] \
    || die "default multilib libgcc was not built ($DEFAULT_LIBGCC); MULTILIB_REQUIRED may have pruned it"

# ----------------------------------------------------------------- cleanup ---
if [ "${KEEP_BUILD_DIRS:-0}" != "1" ]; then
    log "removing build trees (set KEEP_BUILD_DIRS=1 to retain)"
    rm -rf "$BUILD/binutils" "$BUILD/gcc"
fi

log "done. Toolchain installed to $PREFIX"
echo
echo "Add it to your PATH:"
echo "    export PATH=\"$PREFIX/bin:\$PATH\""
