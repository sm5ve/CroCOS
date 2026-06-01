#!/usr/bin/env bash
# profile_qemu.sh
#
# Profile the CroCOS kernel under QEMU/TCG and pretty-print the top hotspots
# (basic-block execution counts + guest-page access counts) with kernel symbol
# annotation.
#
# Default mode uses our region-of-interest plugin (tools/qemu_plugins/
# croco_profile.c): counting is armed only once guest execution reaches a kernel
# symbol (default: vmsmallocStress), so firmware (SeaBIOS) and kernel boot/init
# are excluded. Use --from-boot to profile the whole run, or --upstream to use
# the stock QEMU hotblocks/hotpages plugins instead (downloaded + compiled,
# whole-run only).
#
# Usage:
#   ./tools/profile_qemu.sh [options]
#
# Options / environment overrides:
#   -d, --duration N      timeout safety net in seconds (default: 90). The
#                         kernel normally self-shuts-down well before this.
#   -n, --top N           entries per summary (default: 20)
#   -r, --roi-symbol SYM  arm profiling when this kernel symbol first executes
#                         (substring match on the demangled name; default
#                         "vmsmallocStress")
#       --from-boot       profile the whole run (no ROI gate)
#       --upstream        use stock hotblocks/hotpages plugins (whole-run)
#       --rebuild-plugins force re-download/recompile of plugins
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
ROI_SYMBOL="vmsmallocStress"
FROM_BOOT=0
USE_UPSTREAM=0
REBUILD=0
NOTE=""
KERNEL="${KERNEL:-cmake-build-debug/kernel/Kernel}"
KERNEL_DEBUG="${KERNEL_DEBUG:-cmake-build-debug/kernel/Kernel.debug}"
SMP="${SMP:-8}"
MEM="${MEM:-256M}"
CPU="${CPU:-qemu64,+fsgsbase}"
QEMU_PLUGIN_INCLUDE="${QEMU_PLUGIN_INCLUDE:-/usr/local/include}"
CC="${CC:-cc}"
QEMU="${QEMU:-qemu-system-x86_64}"
NM="${NM:-x86_64-elf-nm}"
CXXFILT="${CXXFILT:-x86_64-elf-c++filt}"

PROFILE_DIR="$REPO_ROOT/profile"
PLUGIN_DIR="$PROFILE_DIR/plugins"
SRC_DIR="$PLUGIN_DIR/src"
CROCO_SRC="$SCRIPT_DIR/qemu_plugins/croco_profile.c"
RAW_LOG="$PROFILE_DIR/plugin_raw.log"
SERIAL_LOG="$PROFILE_DIR/serial.log"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--duration)       DURATION="$2"; shift 2 ;;
        -n|--top)            TOP="$2";      shift 2 ;;
        -r|--roi-symbol)     ROI_SYMBOL="$2"; shift 2 ;;
        --from-boot)         FROM_BOOT=1;   shift   ;;
        --upstream)          USE_UPSTREAM=1; shift  ;;
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
[[ -f "$KERNEL_DEBUG" ]] || echo "warning: $KERNEL_DEBUG missing — symbols will be unresolved." >&2
[[ -f "$QEMU_PLUGIN_INCLUDE/qemu-plugin.h" ]] \
    || err "qemu-plugin.h not found in $QEMU_PLUGIN_INCLUDE (set QEMU_PLUGIN_INCLUDE)."
command -v pkg-config >/dev/null 2>&1 || err "pkg-config not found (needed for glib flags)."
pkg-config --exists glib-2.0 || err "glib-2.0 not found via pkg-config."

QEMU_VERSION="$("$QEMU" --version | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p' | head -1)"
[[ -n "$QEMU_VERSION" ]] || err "could not parse QEMU version."
QEMU_TAG="v$QEMU_VERSION"
mkdir -p "$SRC_DIR"

# Compile a single plugin .c into a .dylib (standalone, against qemu-plugin.h + glib).
compile_plugin() {
    local src="$1" out="$2"
    # shellcheck disable=SC2046
    "$CC" -dynamiclib -fPIC -O2 \
        -I"$QEMU_PLUGIN_INCLUDE" $(pkg-config --cflags glib-2.0) \
        "$src" -o "$out" \
        $(pkg-config --libs glib-2.0) -Wl,-undefined,dynamic_lookup \
        || err "failed to compile $(basename "$out")"
    nm -gU "$out" 2>/dev/null | grep -q qemu_plugin_install \
        || err "$(basename "$out") does not export qemu_plugin_install."
}

# Resolve a kernel symbol (substring of its demangled name) to a hex address.
resolve_symbol() {
    local query="$1"
    [[ -f "$KERNEL_DEBUG" ]] || err "need $KERNEL_DEBUG to resolve ROI symbol '$query'."
    local table addr
    table="$("$NM" -n "$KERNEL_DEBUG" 2>/dev/null | awk '$2 ~ /^[tTwW]$/ {print $1"\t"$3}')"
    addr="$(paste \
        <(cut -f1 <<<"$table") \
        <(cut -f2 <<<"$table" | "$CXXFILT") \
        | grep -F "$query" | head -1 | cut -f1)"
    [[ -n "$addr" ]] || err "ROI symbol '$query' not found in $KERNEL_DEBUG (try --from-boot, or a different -r)."
    echo "0x$addr"
}

# ── Build plugins + assemble -plugin args for the chosen mode ────────────────
PLUGIN_FLAGS=()
if [[ "$USE_UPSTREAM" -eq 1 ]]; then
    STAMP="$PLUGIN_DIR/.qemu_version"
    need=$REBUILD
    [[ -f "$PLUGIN_DIR/libhotblocks.dylib" && -f "$PLUGIN_DIR/libhotpages.dylib" ]] || need=1
    [[ -f "$STAMP" && "$(cat "$STAMP" 2>/dev/null)" == "$QEMU_VERSION" ]] || need=1
    if [[ "$need" -eq 1 ]]; then
        echo "── Building upstream hotblocks/hotpages for QEMU $QEMU_VERSION ──"
        for p in hotblocks hotpages; do
            src="$SRC_DIR/$p.c"
            if [[ ! -f "$src" || "$REBUILD" -eq 1 ]]; then
                url="https://gitlab.com/qemu-project/qemu/-/raw/$QEMU_TAG/contrib/plugins/$p.c"
                echo "   downloading $p.c ($QEMU_TAG)"
                curl -fsSL "$url" -o "$src" || err "failed to download $url"
            fi
            echo "   compiling lib$p.dylib"
            compile_plugin "$src" "$PLUGIN_DIR/lib$p.dylib"
        done
        echo "$QEMU_VERSION" > "$STAMP"
    else
        echo "── Using cached upstream plugins (QEMU $QEMU_VERSION) ──"
    fi
    PLUGIN_FLAGS=(-plugin "$PLUGIN_DIR/libhotblocks.dylib,inline=off"
                  -plugin "$PLUGIN_DIR/libhotpages.dylib,io=off")
    echo "   mode: upstream plugins (whole-run, no ROI)"
    NOTE="Whole-run profile (upstream plugins) — includes firmware (SeaBIOS) + kernel boot/init."
else
    DYLIB="$PLUGIN_DIR/libcroco_profile.dylib"
    if [[ "$REBUILD" -eq 1 || ! -f "$DYLIB" || "$CROCO_SRC" -nt "$DYLIB" ]]; then
        echo "── Building croco_profile plugin ──"
        mkdir -p "$PLUGIN_DIR"
        compile_plugin "$CROCO_SRC" "$DYLIB"
    else
        echo "── Using cached croco_profile plugin ──"
    fi
    args="io=off"
    if [[ "$FROM_BOOT" -eq 1 ]]; then
        echo "   mode: croco_profile (whole-run, no ROI)"
        NOTE="Whole-run profile — includes firmware (SeaBIOS) + kernel boot/init."
    else
        roi_addr="$(resolve_symbol "$ROI_SYMBOL")"
        args="roi_start=$roi_addr,$args"
        echo "   mode: croco_profile (ROI armed at '$ROI_SYMBOL' = $roi_addr)"
        NOTE="Region of interest: counting armed at '$ROI_SYMBOL' ($roi_addr) — firmware (SeaBIOS) + kernel boot/init excluded."
    fi
    PLUGIN_FLAGS=(-plugin "$DYLIB,$args")
fi

# ── Run the kernel under the plugin(s) ───────────────────────────────────────
rm -f "$RAW_LOG" "$SERIAL_LOG"
echo "── Booting kernel under TCG (timeout ${DURATION}s) ──"
echo "   kernel: $KERNEL"
# Single-thread TCG: plugin counters are not multi-thread-safe. The kernel
# self-shuts-down (Shutdown init phase), so QEMU normally exits cleanly and the
# plugin flushes via its atexit callback; the timeout is only a safety net for a
# hang (SIGTERM still triggers a graceful shutdown + flush).
set +e
timeout "$DURATION" "$QEMU" \
    -accel tcg,thread=single \
    -smp "$SMP" -m "$MEM" -cpu "$CPU" \
    -no-reboot -display none -monitor none \
    "${PLUGIN_FLAGS[@]}" \
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
  mismatch. Rebuild against the running QEMU:
      $QEMU --version
      $0 --rebuild-plugins"
fi

# ── Parse + report ───────────────────────────────────────────────────────────
echo "── Top $TOP hotspots ──"
python3 "$SCRIPT_DIR/qemu_profile_report.py" \
    --log "$RAW_LOG" \
    --kernel-debug "$KERNEL_DEBUG" \
    --nm "$NM" --cxxfilt "$CXXFILT" \
    --out-dir "$PROFILE_DIR" \
    --top "$TOP" \
    --note "$NOTE"

echo "── Files written under $PROFILE_DIR/ ──"
echo "   plugin_raw.log  hotblocks.txt  hotpages.txt"
echo "   hotblocks_summary.txt  hotpages_summary.txt  serial.log"
