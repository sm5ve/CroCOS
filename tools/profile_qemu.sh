#!/usr/bin/env bash
# profile_qemu.sh
#
# Profile the CroCOS kernel under QEMU/TCG using the upstream hotblocks
# (basic-block execution counts) and hotpages (guest-page access counts) TCG
# plugins, then pretty-print the top hotspots with kernel symbol annotation.
#
# On first run it downloads the version-matched plugin sources from the official
# QEMU mirror and compiles them standalone against the installed qemu-plugin.h
# (cached under profile/plugins/). Subsequent runs reuse the cached dylibs.
#
# Usage:
#   ./tools/profile_qemu.sh [-d SECONDS] [-n TOP] [--rebuild-plugins]
#
# Options / environment overrides:
#   -d, --duration N      timeout safety net in seconds (default: 90). The
#                         kernel normally self-shuts-down well before this.
#   -n, --top N           entries per summary (default: 20)
#       --rebuild-plugins force re-download + recompile of the plugins
#   KERNEL                kernel image    (default: cmake-build-debug/kernel/Kernel)
#   KERNEL_DEBUG          unstripped syms (default: cmake-build-debug/kernel/Kernel.debug)
#   SMP / MEM / CPU       qemu machine    (default: 8 / 256M / qemu64,+fsgsbase)
#   QEMU_PLUGIN_INCLUDE   dir with qemu-plugin.h (default: /usr/local/include)
#   CC                    compiler for the plugins (default: cc)
#
# Output (under profile/): plugin_raw.log, serial.log, hotblocks.txt,
# hotpages.txt, hotblocks_summary.txt, hotpages_summary.txt, plugins/*.dylib.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# ── Defaults / config ────────────────────────────────────────────────────────
DURATION=90
TOP=20
REBUILD=0
KERNEL="${KERNEL:-cmake-build-debug/kernel/Kernel}"
KERNEL_DEBUG="${KERNEL_DEBUG:-cmake-build-debug/kernel/Kernel.debug}"
SMP="${SMP:-8}"
MEM="${MEM:-256M}"
CPU="${CPU:-qemu64,+fsgsbase}"
QEMU_PLUGIN_INCLUDE="${QEMU_PLUGIN_INCLUDE:-/usr/local/include}"
CC="${CC:-cc}"
QEMU="${QEMU:-qemu-system-x86_64}"

PROFILE_DIR="$REPO_ROOT/profile"
PLUGIN_DIR="$PROFILE_DIR/plugins"
SRC_DIR="$PLUGIN_DIR/src"
RAW_LOG="$PROFILE_DIR/plugin_raw.log"
SERIAL_LOG="$PROFILE_DIR/serial.log"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--duration)       DURATION="$2"; shift 2 ;;
        -n|--top)            TOP="$2";      shift 2 ;;
        --rebuild-plugins)   REBUILD=1;     shift   ;;
        -h|--help)           sed -n '2,/^$/p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

err() { echo "ERROR: $*" >&2; exit 1; }

# ── Preflight ────────────────────────────────────────────────────────────────
command -v "$QEMU" >/dev/null 2>&1 || err "$QEMU not found on PATH."
# `-d help` prints to stderr and exits non-zero; capture then match (pipefail-safe).
d_help="$("$QEMU" -d help 2>&1 || true)"
grep -q '^plugin' <<<"$d_help" \
    || err "$QEMU has no plugin support (need a build with --enable-plugins)."
[[ -f "$KERNEL" ]]       || err "kernel image not found: $KERNEL (build it first)."
[[ -f "$KERNEL_DEBUG" ]] || echo "warning: $KERNEL_DEBUG missing — hotblocks symbols will be unresolved." >&2

QEMU_VERSION="$("$QEMU" --version | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p' | head -1)"
[[ -n "$QEMU_VERSION" ]] || err "could not parse QEMU version from '$QEMU --version'."
QEMU_TAG="v$QEMU_VERSION"

mkdir -p "$SRC_DIR"

# ── Build (or reuse) the plugins ─────────────────────────────────────────────
STAMP="$PLUGIN_DIR/.qemu_version"
need_build=$REBUILD
[[ -f "$PLUGIN_DIR/libhotblocks.dylib" && -f "$PLUGIN_DIR/libhotpages.dylib" ]] || need_build=1
[[ -f "$STAMP" && "$(cat "$STAMP" 2>/dev/null)" == "$QEMU_VERSION" ]] || need_build=1

if [[ "$need_build" -eq 1 ]]; then
    echo "── Building hotblocks/hotpages plugins for QEMU $QEMU_VERSION ──"
    [[ -f "$QEMU_PLUGIN_INCLUDE/qemu-plugin.h" ]] \
        || err "qemu-plugin.h not found in $QEMU_PLUGIN_INCLUDE (set QEMU_PLUGIN_INCLUDE)."
    command -v pkg-config >/dev/null 2>&1 || err "pkg-config not found (needed for glib flags)."
    pkg-config --exists glib-2.0 || err "glib-2.0 not found via pkg-config."

    for p in hotblocks hotpages; do
        src="$SRC_DIR/$p.c"
        if [[ ! -f "$src" ]]; then
            url="https://gitlab.com/qemu-project/qemu/-/raw/$QEMU_TAG/contrib/plugins/$p.c"
            echo "   downloading $p.c ($QEMU_TAG)"
            curl -fsSL "$url" -o "$src" || err "failed to download $url"
        fi
        echo "   compiling lib$p.dylib"
        # shellcheck disable=SC2046
        "$CC" -dynamiclib -fPIC -O2 \
            -I"$QEMU_PLUGIN_INCLUDE" $(pkg-config --cflags glib-2.0) \
            "$src" -o "$PLUGIN_DIR/lib$p.dylib" \
            $(pkg-config --libs glib-2.0) -Wl,-undefined,dynamic_lookup \
            || err "failed to compile lib$p.dylib"
        nm -gU "$PLUGIN_DIR/lib$p.dylib" 2>/dev/null | grep -q qemu_plugin_install \
            || err "lib$p.dylib does not export qemu_plugin_install."
    done
    echo "$QEMU_VERSION" > "$STAMP"
else
    echo "── Using cached plugins (QEMU $QEMU_VERSION) ──"
fi

# ── Run the kernel under both plugins ────────────────────────────────────────
rm -f "$RAW_LOG" "$SERIAL_LOG"
echo "── Booting kernel under TCG + plugins (timeout ${DURATION}s) ──"
echo "   kernel: $KERNEL"
# Single-thread TCG: plugin counters are not multi-thread-safe. The kernel
# self-shuts-down (Shutdown init phase), so QEMU normally exits cleanly and the
# plugins flush via their atexit callbacks; the timeout is only a safety net for
# a hang (SIGTERM still triggers a graceful shutdown + flush).
set +e
timeout "$DURATION" "$QEMU" \
    -accel tcg,thread=single \
    -smp "$SMP" -m "$MEM" -cpu "$CPU" \
    -no-reboot -display none -monitor none \
    -plugin "$PLUGIN_DIR/libhotblocks.dylib,inline=off" \
    -plugin "$PLUGIN_DIR/libhotpages.dylib,io=on" \
    -d plugin -D "$RAW_LOG" \
    -serial "file:$SERIAL_LOG" \
    -kernel "$KERNEL"
qrc=$?
set -e
[[ "$qrc" -eq 124 ]] && echo "   (timeout fired — kernel did not self-shutdown; SIGTERM-flushed)"

# ── Validate plugin output ───────────────────────────────────────────────────
if [[ ! -s "$RAW_LOG" ]]; then
    err "plugin log is empty ($RAW_LOG).
  Plugins loaded but produced no output — most likely a plugin API-version
  mismatch. Rebuild them against the running QEMU:
      $QEMU --version
      $0 --rebuild-plugins"
fi

# ── Parse + report ───────────────────────────────────────────────────────────
echo "── Top $TOP hotspots ──"
python3 "$SCRIPT_DIR/qemu_profile_report.py" \
    --log "$RAW_LOG" \
    --kernel-debug "$KERNEL_DEBUG" \
    --out-dir "$PROFILE_DIR" \
    --top "$TOP"

echo "── Files written under $PROFILE_DIR/ ──"
echo "   plugin_raw.log  hotblocks.txt  hotpages.txt"
echo "   hotblocks_summary.txt  hotpages_summary.txt  serial.log"
