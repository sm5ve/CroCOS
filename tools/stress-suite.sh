#!/usr/bin/env bash
#
# Long-running, load-heavy repetition of the concurrency-sensitive test runners.
#
# Why this exists, and what a pass is worth.
#
# The PAI_ConcurrentStress SEGV (fixed by "tests: a thread declares its CPU")
# was invisible to a normal suite run: 0/5 at rest, and it only appeared under
# heavy machine load, because load changes which threads take the mock's
# fresh-assignment path. A serial re-run of the suite therefore proves nothing
# about it. This script reproduces the condition that actually mattered — many
# suite instances competing for the same cores — and repeats it long enough for
# a rare event to show up.
#
# So a pass here is evidence of a *specific* kind: it says the load-dependent
# crash no longer reproduces under the conditions that used to produce it. It is
# not a proof of correctness, and it should be read alongside the two positive
# controls that show the detection works at all:
#   - the pre-fix reproduction (3/3 with the mock's thread-id map deleted), and
#   - ArchMocksTest, whose guards were each verified to go red when broken.
# Run this script's own self-check (--self-check) to confirm that a crashing
# runner is actually caught rather than silently counted as a pass.
#
# TSan runners are deliberately excluded. Sixteen of their tests are wall-clock
# sensitive and starve under exactly the machine load this script creates, so
# including them would manufacture red that means nothing (441 idle vs 425
# loaded). Run `run_tsan_stress` separately, on a quiet machine.
#
# Usage:
#   tools/stress-suite.sh [--minutes N] [--parallel N] [--self-check]
#
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO_ROOT/tests/build"

MINUTES=60
PARALLEL=0          # 0 => auto (2x cores, to oversubscribe)
SELF_CHECK=0
PER_RUN_TIMEOUT=900 # seconds; a hang is a finding, not a reason to wait forever

while [[ $# -gt 0 ]]; do
    case "$1" in
        --minutes)    MINUTES="$2"; shift 2 ;;
        --parallel)   PARALLEL="$2"; shift 2 ;;
        --self-check) SELF_CHECK=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ "$PARALLEL" -eq 0 ]]; then
    CORES="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
    PARALLEL=$(( CORES * 2 ))
fi

# The runners that touch per-CPU state, weighted by how much they exercise it.
# KernelTestRunner appears twice: it owns the page allocator, which is where the
# original crash lived.
RUNNERS=(
    "$BUILD/kernel/KernelTestRunner"
    "$BUILD/kernel/KernelTestRunner"
    "$BUILD/kernel/radix/KernelRadixTestRunner"
    "$BUILD/kernel/rcu/KernelRcuIntegrationTestRunner"
    "$BUILD/kernel/vmsmalloc/KernelVmsmallocIntegrationTestRunner"
    "$BUILD/core/CoreTestRunner"
    "$BUILD/liballoc/LibAllocTestRunner"
)

for r in "${RUNNERS[@]}"; do
    if [[ ! -x "$r" ]]; then
        echo "missing runner: $r" >&2
        echo "build first:  cd tests && cmake --build build -j8" >&2
        exit 2
    fi
done

LOGDIR="${TMPDIR:-/tmp}/crocos-stress-$$"
mkdir -p "$LOGDIR"

echo "=== CroCOS stress harness ==="
echo "commit    : $(cd "$REPO_ROOT" && git rev-parse --short HEAD) on $(cd "$REPO_ROOT" && git rev-parse --abbrev-ref HEAD)"
echo "duration  : ${MINUTES} min"
echo "parallel  : ${PARALLEL} concurrent runners"
echo "logs      : $LOGDIR"
echo

# ── failure classification ─────────────────────────────────────────────────
# These runners are ASan-built, so a SEGV is reported BY ASan and exits 1 — the
# shell never sees 139. Classifying on the exit code alone would file a memory
# fault as an ordinary test failure, so the output is what decides.
classify() {
    local exitcode="$1" logfile="$2"
    if grep -qE "SEGV on unknown address|SEGV_|SIGSEGV" "$logfile" 2>/dev/null; then
        echo "SEGV"
    elif grep -qE "heap-use-after-free|heap-buffer-overflow|stack-buffer|double-free" "$logfile" 2>/dev/null; then
        echo "ASAN-MEMORY"
    elif grep -qE "LeakSanitizer|detected memory leaks" "$logfile" 2>/dev/null; then
        echo "LEAK"
    elif grep -qE "two live threads bound to one logical CPU|no ProcessorBinding" "$logfile" 2>/dev/null; then
        echo "CPU-EXCLUSIVITY"
    elif grep -qE "residue <= kResidueBound" "$logfile" 2>/dev/null; then
        # rcuTortureDeadSlotDoesNotUnboundLimbo. A pre-existing, already-tracked
        # red that this harness reproduces readily under load. Named separately
        # so it neither masquerades as a regression of the bug under test nor
        # gets quietly dropped — it is still an open failure, counted below.
        echo "KNOWN-OPEN-RCU-RESIDUE"
    elif [[ "$exitcode" -eq 124 ]]; then
        echo "TIMEOUT"
    elif grep -qE "Test timed out after" "$logfile" 2>/dev/null; then
        echo "TEST-TIMEOUT"
    elif [[ "$exitcode" -eq 139 || "$exitcode" -eq 134 ]]; then
        echo "CRASH-$exitcode"
    else
        echo "TEST-FAILURE"
    fi
}

# ── self-check: prove the harness can see a crash ──────────────────────────
if [[ "$SELF_CHECK" -eq 1 ]]; then
    echo "self-check: proving a crash is not silently counted as a pass..."
    cat > "$LOGDIR/crasher.c" <<'EOF'
int main(void) { volatile int *p = 0; *p = 1; return 0; }
EOF
    selfcheck_failed=0

    # (a) Bare crash: the shell sees 128+SIGSEGV.
    cc -o "$LOGDIR/crasher" "$LOGDIR/crasher.c" 2>/dev/null
    "$LOGDIR/crasher" > "$LOGDIR/selfcheck-bare.log" 2>&1
    rc=$?
    kind="$(classify "$rc" "$LOGDIR/selfcheck-bare.log")"
    echo "  bare crash        : exit=$rc -> $kind"
    [[ "$kind" == "TEST-FAILURE" ]] && selfcheck_failed=1

    # (b) The shape that actually matters. Every runner here is ASan-built, so
    #     ASan intercepts the fault, prints its own report and exits 1 — the
    #     exit code alone is indistinguishable from an ordinary failing test.
    #     If this case is misclassified, a real SEGV would be filed as a test
    #     failure and the verdict line would still read plausibly.
    if cc -fsanitize=address -o "$LOGDIR/crasher-asan" "$LOGDIR/crasher.c" 2>/dev/null; then
        "$LOGDIR/crasher-asan" > "$LOGDIR/selfcheck-asan.log" 2>&1
        rc=$?
        kind="$(classify "$rc" "$LOGDIR/selfcheck-asan.log")"
        echo "  ASan-reported SEGV: exit=$rc -> $kind"
        if [[ "$kind" != "SEGV" ]]; then
            echo "  ^ misclassified: expected SEGV" >&2
            selfcheck_failed=1
        fi
    else
        echo "  ASan-reported SEGV: SKIPPED (no ASan-capable cc)" >&2
        selfcheck_failed=1
    fi

    if [[ "$selfcheck_failed" -ne 0 ]]; then
        echo "self-check FAILED — a passing verdict from this harness would be worthless." >&2
        exit 1
    fi
    echo "self-check passed: crashes are detected in both forms."
    echo
fi

# ── the loop ───────────────────────────────────────────────────────────────
END=$(( $(date +%s) + MINUTES * 60 ))
iterations=0
failures=0
declare -a FAILKINDS=()

trap 'echo; echo "interrupted — reporting what ran so far"; report; exit 130' INT TERM

report() {
    local known=0 regressions=0
    if [[ "${#FAILKINDS[@]}" -gt 0 ]]; then
        known=$(printf '%s\n' "${FAILKINDS[@]}" | grep -c "KNOWN-OPEN" || true)
        regressions=$(( failures - known ))
    fi

    echo
    echo "=== result ==="
    echo "commit      : $(cd "$REPO_ROOT" && git rev-parse --short HEAD)"
    echo "runner-runs : $iterations"
    echo "failures    : $failures  (regressions: $regressions, known-open: $known)"
    if [[ "$failures" -gt 0 ]]; then
        printf '%s\n' "${FAILKINDS[@]}" | sort | uniq -c | sort -rn
        echo "logs kept in: $LOGDIR"
    fi
    echo

    # The question this harness exists to answer.
    if [[ "$regressions" -eq 0 ]]; then
        echo "VERDICT (crash/exclusivity): PASS — no SEGV, memory error or"
        echo "CPU-exclusivity violation in $iterations runner-runs at parallelism $PARALLEL."
    else
        echo "VERDICT (crash/exclusivity): FAIL — $regressions regression(s)."
    fi

    # Stated separately and always, so a green verdict above can never be read
    # as "the suite is green".
    if [[ "$known" -gt 0 ]]; then
        echo
        echo "STILL RED (pre-existing, not fixed by this work): $known occurrence(s)"
        echo "of rcuTortureDeadSlotDoesNotUnboundLimbo. This run is NOT fully green."
    fi

    [[ "$failures" -eq 0 ]] && rm -rf "$LOGDIR"
    return $(( regressions > 0 ? 1 : 0 ))
}

batch=0
while [[ $(date +%s) -lt $END ]]; do
    batch=$(( batch + 1 ))
    pids=()
    logs=()

    for (( i = 0; i < PARALLEL; i++ )); do
        runner="${RUNNERS[$(( (batch * PARALLEL + i) % ${#RUNNERS[@]} ))]}"
        log="$LOGDIR/b${batch}-i${i}-$(basename "$runner").log"
        logs+=("$log:$runner")
        # detect_leaks is already on in these builds; keep ASan's own exit code
        # so classify() sees both the report and the status.
        ( timeout "$PER_RUN_TIMEOUT" "$runner" > "$log" 2>&1; echo $? > "$log.rc" ) &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do wait "$pid"; done

    for entry in "${logs[@]}"; do
        log="${entry%%:*}"
        runner="${entry#*:}"
        rc="$(cat "$log.rc" 2>/dev/null || echo 1)"
        iterations=$(( iterations + 1 ))
        if [[ "$rc" -ne 0 ]]; then
            kind="$(classify "$rc" "$log")"
            FAILKINDS+=("$kind  $(basename "$runner")")
            failures=$(( failures + 1 ))
            echo "  !! $kind in $(basename "$runner") (exit $rc) -> $log"
        else
            rm -f "$log" "$log.rc"
        fi
    done

    remaining=$(( (END - $(date +%s)) / 60 ))
    echo "batch $batch done — $iterations runner-runs, $failures failures, ~${remaining} min left"
done

report
exit $?
