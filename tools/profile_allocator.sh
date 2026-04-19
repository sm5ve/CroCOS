#!/usr/bin/env bash
# profile_allocator.sh
#
# Build PageAllocatorStress, profile it for DURATION seconds with macOS `sample`,
# then post-process the results into a flamegraph collapsed-stack report.
#
# Usage:
#   ./tools/profile_allocator.sh [options] [-- <PageAllocatorStress args>]
#
# Options:
#   -d, --duration N    Sample duration in seconds (default: 30)
#   -w, --warmup N      Warmup seconds before sampling starts (default: 2)
#   -o, --output DIR    Directory for output files (default: /tmp/pa_profile)
#   -n, --min-count N   Suppress flamegraph entries below N samples (default: 5)
#       --no-build      Skip rebuild (use existing binary)
#       --app-only      Filter system frames from flamegraph output
#
# Everything after -- is passed verbatim to PageAllocatorStress.
# If no stress-test args are given, the binary runs with its defaults, but
# --intervals is automatically set to keep the run time bounded.
#
# Example:
#   ./tools/profile_allocator.sh -d 20 -- --threads 8 --pages 256

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STRESS_DIR="$REPO_ROOT/stress_tests"
BINARY="$STRESS_DIR/build/PageAllocatorStress"
FLAMEGRAPH_SCRIPT="$SCRIPT_DIR/sample_to_flamegraph.py"

# ── Defaults ────────────────────────────────────────────────────────────────
DURATION=30
WARMUP=2
OUTPUT_DIR=/tmp/pa_profile
MIN_COUNT=5
DO_BUILD=1
APP_ONLY=
STRESS_ARGS=()

# ── Argument parsing ─────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--duration)   DURATION="$2";   shift 2 ;;
        -w|--warmup)     WARMUP="$2";     shift 2 ;;
        -o|--output)     OUTPUT_DIR="$2"; shift 2 ;;
        -n|--min-count)  MIN_COUNT="$2";  shift 2 ;;
        --no-build)      DO_BUILD=0;      shift   ;;
        --app-only)      APP_ONLY="--app-only"; shift ;;
        --)              shift; STRESS_ARGS=("$@"); break ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUTPUT_DIR"
SAMPLE_FILE="$OUTPUT_DIR/sample_${TIMESTAMP}.txt"
COLLAPSED_FILE="$OUTPUT_DIR/collapsed_${TIMESTAMP}.txt"

# ── Build ────────────────────────────────────────────────────────────────────
if [[ $DO_BUILD -eq 1 ]]; then
    echo "── Building PageAllocatorStress ──────────────────────────────────────"
    cd "$STRESS_DIR"
    if [[ ! -f build/CMakeCache.txt ]]; then
        cmake -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev 2>&1
    fi
    cmake --build build --target PageAllocatorStress 2>&1
    cd "$REPO_ROOT"
    echo
fi

if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: binary not found: $BINARY" >&2
    exit 1
fi

# ── Compute total run duration and --intervals ───────────────────────────────
# The stress test's report interval is 1 000 ms during profiling (gives fine
# progress output without adding overhead).  We add enough intervals to cover
# warmup + sample duration + one extra interval as a margin.
INTERVAL_MS=1000
TOTAL_SECONDS=$(( WARMUP + DURATION + INTERVAL_MS / 1000 + 1 ))
MAX_INTERVALS=$(( TOTAL_SECONDS * 1000 / INTERVAL_MS ))

# Inject timing args unless the caller already supplied --intervals / --interval
TIMING_ARGS=(--interval "$INTERVAL_MS" --intervals "$MAX_INTERVALS")
for arg in "${STRESS_ARGS[@]}"; do
    if [[ "$arg" == "--interval" || "$arg" == "--intervals" ]]; then
        TIMING_ARGS=()   # caller controls timing
        break
    fi
done

FINAL_STRESS_ARGS=("${TIMING_ARGS[@]}" "${STRESS_ARGS[@]}")

# ── Launch stress test ───────────────────────────────────────────────────────
echo "── Launching PageAllocatorStress ─────────────────────────────────────"
echo "   Args: ${FINAL_STRESS_ARGS[*]}"
echo "   Warmup: ${WARMUP}s  |  Sample duration: ${DURATION}s"
echo "   Output directory: $OUTPUT_DIR"
echo

"$BINARY" "${FINAL_STRESS_ARGS[@]}" &
STRESS_PID=$!

# Ensure the stress test is killed if this script exits early
cleanup() {
    kill "$STRESS_PID" 2>/dev/null || true
}
trap cleanup EXIT

# ── Warmup ───────────────────────────────────────────────────────────────────
if [[ $WARMUP -gt 0 ]]; then
    echo "── Warming up for ${WARMUP}s… ───────────────────────────────────────"
    sleep "$WARMUP"
fi

# Verify the process is still alive
if ! kill -0 "$STRESS_PID" 2>/dev/null; then
    echo "ERROR: PageAllocatorStress exited before profiling started." >&2
    exit 1
fi

# ── Sample ───────────────────────────────────────────────────────────────────
echo "── Sampling PID $STRESS_PID for ${DURATION}s → $SAMPLE_FILE ─────────"
sample "$STRESS_PID" "$DURATION" -file "$SAMPLE_FILE"
echo

# ── Wait for stress test to finish ───────────────────────────────────────────
echo "── Waiting for stress test to finish… ────────────────────────────────"
wait "$STRESS_PID" 2>/dev/null || true
trap - EXIT   # disarm cleanup

# ── Post-process ─────────────────────────────────────────────────────────────
echo
echo "── Analyzing profile → $COLLAPSED_FILE ──────────────────────────────"
python3 "$FLAMEGRAPH_SCRIPT" \
    --min-count "$MIN_COUNT" \
    ${APP_ONLY} \
    "$SAMPLE_FILE" \
    > "$COLLAPSED_FILE"

echo
echo "── Files ─────────────────────────────────────────────────────────────"
echo "   Raw sample:  $SAMPLE_FILE"
echo "   Collapsed:   $COLLAPSED_FILE"
echo
echo "   To generate an SVG flamegraph (requires flamegraph.pl):"
echo "   flamegraph.pl $COLLAPSED_FILE > $OUTPUT_DIR/flamegraph_${TIMESTAMP}.svg"
echo
