#!/bin/bash
# Redis Stream Workload Benchmark
#
# Measures XADD (insert) and XRANGE (query) performance for vanilla rax vs
# QuART (rax_quart) as Redis's internal stream index, across bods workloads
# with varying sortedness (K).  Each uint32 value is used as both stream ID
# and field value.
#
#   RAX   – redis-server started with stream-quart-enabled no  (default)
#   QuART – same redis-server binary, started with stream-quart-enabled yes
#
#   K=0   fully sorted   → one sub-stream, every XADD hits QuART fast-path
#   K=100 fully random   → many sub-streams, no fast-path benefit
#
# Usage:
#   ./test_redis_workloads.sh [--rax-bin /path/redis-server]
#                             [--quart-bin /path/redis-server]  # same binary
#                             [--skip-rax] [--skip-quart]
#                             [--port PORT] [--N num_keys]
#                             [--xrange-count N]
#
# Results: results/redis_stream_test_<timestamp>.csv
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── default paths ──────────────────────────────────────────────────────────
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_RAX_BIN="$REPO_ROOT/src/redis-server"
DEFAULT_QUART_BIN="$REPO_ROOT/src/redis-server"  # same binary; QuART is a runtime config flag
TEST_PROG="$REPO_ROOT/src/test-redis-stream-workloads"
WORKLOAD_DIR="/home/grad1/cgokmen/bods/workloads/"  # same workloads as standalone test

# ── configuration ─────────────────────────────────────────────────────────
REDIS_PORT=7379
N=50000000         # number of keys to actually insert/query (first N from file)
WORKLOAD_N=500000000  # N embedded in the workload filenames
REPEAT=3
KL_VALUES=(0)
XRANGE_COUNT=1000   # max entries per XRANGE reply (caps reply size for sorted workloads)
RESULTSDIR="results"
LOGDIR="$RESULTSDIR/logs"

RAX_BIN="$DEFAULT_RAX_BIN"
QUART_BIN="$DEFAULT_QUART_BIN"
SKIP_RAX=0
SKIP_QUART=0

# ── parse args ─────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rax-bin)   RAX_BIN="$2";   shift 2 ;;
        --quart-bin) QUART_BIN="$2"; shift 2 ;;
        --skip-rax)   SKIP_RAX=1;    shift   ;;
        --skip-quart) SKIP_QUART=1;  shift   ;;
        --port)        REDIS_PORT="$2";   shift 2 ;;
        --N)           N="$2";             shift 2 ;;
        --workload-n)  WORKLOAD_N="$2";    shift 2 ;;
        --xrange-count) XRANGE_COUNT="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

SUFFIX=$(date +"%Y%m%d_%H%M%S")
RESULTS="${RESULTSDIR}/redis_stream_test_${SUFFIX}.csv"
mkdir -p "$LOGDIR"

# ── sanity checks ──────────────────────────────────────────────────────────
if [[ ! -f "$TEST_PROG" ]]; then
    echo "Building test-redis-stream-workloads..."
    (cd "$REPO_ROOT/src" && make test-redis-stream-workloads)
    if [[ $? -ne 0 ]]; then
        echo "ERROR: build failed"
        exit 1
    fi
    echo "Build OK"
    echo ""
fi

if [[ $SKIP_RAX -eq 0 && ! -f "$RAX_BIN" ]]; then
    echo "WARNING: RAX server binary not found at $RAX_BIN"
    echo "         Pass --rax-bin or --skip-rax to suppress this."
    SKIP_RAX=1
fi

if [[ $SKIP_QUART -eq 0 && ! -f "$QUART_BIN" ]]; then
    echo "WARNING: Redis server binary not found at $QUART_BIN — skipping QuART"
    SKIP_QUART=1
fi

if [[ $SKIP_RAX -eq 1 && $SKIP_QUART -eq 1 ]]; then
    echo "ERROR: both RAX and QuART are skipped – nothing to test."
    exit 1
fi

# ── helpers ────────────────────────────────────────────────────────────────

REDIS_PID=""
REDIS_PIDFILE="/tmp/redis_test_${REDIS_PORT}.pid"

start_redis() {
    local binary="$1"
    local use_quart="${2:-no}"
    local conf="${LOGDIR}/redis_${SUFFIX}.conf"

    # Kill anything still on the port before starting
    local stale
    stale=$(fuser "${REDIS_PORT}/tcp" 2>/dev/null | tr -s ' ' '\n' | grep -v '^$' | head -1 || true)
    if [[ -n "$stale" ]]; then
        kill "$stale" 2>/dev/null || true
        sleep 0.5
    fi

    cat > "$conf" <<EOF
port $REDIS_PORT
daemonize yes
logfile ${LOGDIR}/redis_${SUFFIX}.log
pidfile $REDIS_PIDFILE

# ── Persistence completely OFF – insert performance must be the sole bottleneck ──

# Snapshotting / RDB off: no periodic saves, no fork, no copy-on-write shadow paging
save ""
rdbchecksum no
rdbcompression no
stop-writes-on-bgsave-error no

# Shadow paging off: BGSAVE forks a child that uses CoW to write a shadow copy of
# the dataset.  With save "" there is no automatic trigger, but we also explicitly
# prevent any BGSAVE error from stalling writes.
# (RDB settings above ensure even an accidental BGSAVE is as cheap as possible.)

# AOF (append-only-file) logging off: no per-command disk writes, no fsync calls
appendonly no
aof-use-rdb-preamble no
no-appendfsync-on-rewrite yes

# ── Stream index selection ────────────────────────────────────────────────────
stream-quart-enabled $use_quart
# Force every XADD to create its own rax node so that rax/QuART
# insertions dominate runtime rather than listpack encoding overhead.
stream-node-max-entries 1
stream-node-max-bytes 1

# Allow DEBUG commands (needed for DEBUG STREAM-BULK-INSERT)
enable-debug-command yes
EOF

    "$binary" "$conf"

    # Wait until Redis responds (up to 5 s)
    local tries=0
    while ! "$REPO_ROOT/src/redis-cli" -p "$REDIS_PORT" PING > /dev/null 2>&1; do
        sleep 0.2
        tries=$((tries + 1))
        if [[ $tries -gt 25 ]]; then
            echo "ERROR: Redis did not start in time"
            return 1
        fi
    done

    REDIS_PID=$(cat "$REDIS_PIDFILE" 2>/dev/null || \
                "$REPO_ROOT/src/redis-cli" -p "$REDIS_PORT" INFO server \
                | grep "^process_id:" | cut -d: -f2 | tr -d ' \r')
}

stop_redis() {
    # Graceful shutdown first
    "$REPO_ROOT/src/redis-cli" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true
    sleep 0.5
    # If the pidfile PID is still alive, kill it
    if [[ -n "$REDIS_PID" ]] && kill -0 "$REDIS_PID" 2>/dev/null; then
        kill "$REDIS_PID" 2>/dev/null || true
        sleep 0.3
    fi
    # Last resort: free the port directly
    fuser -k "${REDIS_PORT}/tcp" 2>/dev/null || true
    rm -f "$REDIS_PIDFILE"
}

# Run a single trial: one Redis start/stop for the given binary, label, K, and run number.
run_one_trial() {
    local binary="$1"
    local label="$2"
    local KL="$3"
    local run="$4"
    local K=$KL
    local L=$KL
    local WORKLOAD="${WORKLOAD_DIR}/workload_N${WORKLOAD_N}_K${K}_L${L}.bin"
    local LOGFILE="${LOGDIR}/log_${label}_K${K}_L${L}_${SUFFIX}.txt"

    printf "    [%s] Run %d/%d: " "$label" "$run" "$REPEAT"

    # Start a fresh Redis instance for each trial to avoid state carry-over
    # Pass stream-quart-enabled yes for QuART runs, no for RAX runs
    local use_quart="no"
    [[ "$label" == "QuART" ]] && use_quart="yes"
    start_redis "$binary" "$use_quart" || { echo "FAIL (server start)"; return; }

    # stdout → tee to terminal + tmpfile (CSV parsing later)
    # stderr → appended to logfile (errors/warnings)
    local TMPOUT
    TMPOUT=$(mktemp)
    echo "" # newline before verbose block
    {
        echo "=== $label K=$K L=$L Run $run/$REPEAT — $(date) ==="
    } >> "$LOGFILE"

    "$TEST_PROG" \
        -f "$WORKLOAD" \
        -N "$N" \
        -h 127.0.0.1 \
        -p "$REDIS_PORT" \
        -n "$label" \
        -C "$XRANGE_COUNT" \
        -v \
        2>>"$LOGFILE" | tee "$TMPOUT" || true

    stop_redis

    local OUTPUT
    OUTPUT=$(cat "$TMPOUT")
    rm -f "$TMPOUT"

    # Append captured stdout to log
    { echo "$OUTPUT"; echo ""; } >> "$LOGFILE"

    # Extract CSV line
    local LINE
    LINE=$(echo "$OUTPUT" | grep "^${label},")

    if [[ -z "$LINE" ]]; then
        echo "FAIL (no CSV output)"
        return
    fi

    local INSERT_NS QUERY_NS
    INSERT_NS=$(echo "$LINE" | cut -d',' -f2)
    QUERY_NS=$(echo "$LINE"  | cut -d',' -f3)

    echo "$N,$K,$L,$label,$run,$INSERT_NS,$QUERY_NS" >> "$RESULTS"
    echo "OK  (insert=${INSERT_NS}ns  query=${QUERY_NS}ns)"
}

# ── CSV header ─────────────────────────────────────────────────────────────
echo "N,K,L,tree_type,run,insert_ns,query_ns" > "$RESULTS"

# ── banner ─────────────────────────────────────────────────────────────────
echo "=========================================================="
echo "Redis Stream Workload Benchmark"
echo "=========================================================="
echo "N=$N keys (first $N of $WORKLOAD_N)  |  REPEAT=$REPEAT  |  PORT=$REDIS_PORT  |  XRANGE COUNT=$XRANGE_COUNT"
echo ""
echo "Workloads (K,L):"
echo "  K=0   fully sorted   -> one sub-stream, QuART fast-path on every XADD"
echo "  K=1   K=5   K=10   K=25   increasing disorder"
echo "  K=100 fully random  -> many sub-streams, no fast-path benefit"
echo ""
echo "Results: $RESULTS"
echo "=========================================================="

# ── run tests (interleaved: RAX run N then QuART run N per workload) ───────
for KL in "${KL_VALUES[@]}"; do
    K=$KL; L=$KL
    WORKLOAD="${WORKLOAD_DIR}/workload_N${WORKLOAD_N}_K${K}_L${L}.bin"
    if [[ ! -f "$WORKLOAD" ]]; then
        echo "  WARNING: workload not found: $WORKLOAD — skipping K=$K"
        continue
    fi
    echo ""
    echo "  K=$KL  workload: $(basename "$WORKLOAD")"
    for ((run=1; run<=REPEAT; run++)); do
        [[ $SKIP_RAX   -eq 0 ]] && run_one_trial "$RAX_BIN"   "RAX"   "$KL" "$run"
        [[ $SKIP_QUART -eq 0 ]] && run_one_trial "$QUART_BIN" "QuART" "$KL" "$run"
    done
    echo ""
done

echo "=========================================================="
echo "Done!  Results: $RESULTS"
echo "=========================================================="

# ── summary ────────────────────────────────────────────────────────────────
echo ""
echo "Generating summary..."
SUMMARY="${RESULTSDIR}/redis_stream_summary_${SUFFIX}.txt"

{
    echo "Redis Stream Workload Benchmark Summary"
    echo "Generated: $(date)"
    echo "N=$N keys (first $N of $WORKLOAD_N)  |  REPEAT=$REPEAT  |  PORT=$REDIS_PORT"
    echo "INSERT=XADD stream:<idx> <k>-<seq> v <k>  |  QUERY=XRANGE COUNT $XRANGE_COUNT"
    echo "========================================================"
    printf "%-8s | %-10s | %15s | %15s\n" "K,L" "Tree" "insert_ns(avg)" "query_ns(avg)"
    printf '%0.s-' {1..60}; echo ""

    for KL in "${KL_VALUES[@]}"; do
        case "$KL" in
            0)   COND="K=0 (fully sorted)" ;;
            1)   COND="K=1" ;;
            5)   COND="K=5" ;;
            10)  COND="K=10" ;;
            25)  COND="K=25" ;;
            100) COND="K=100 (fully random)" ;;
            *)   COND="K=$KL" ;;
        esac

        for LABEL in RAX QuART; do
            AVG_INS=$(grep "^${N},${KL},${KL},${LABEL}," "$RESULTS" \
                | cut -d',' -f6 \
                | awk '{s+=$1;c++} END{if(c>0)printf "%.0f",s/c; else print "N/A"}')
            AVG_QRY=$(grep "^${N},${KL},${KL},${LABEL}," "$RESULTS" \
                | cut -d',' -f7 \
                | awk '{s+=$1;c++} END{if(c>0)printf "%.0f",s/c; else print "N/A"}')

            [[ -z "$AVG_INS" ]] && AVG_INS="N/A"
            [[ -z "$AVG_QRY" ]] && AVG_QRY="N/A"

            printf "%-8s | %-10s | %15s | %15s\n" \
                "$KL,$KL" "$LABEL" "$AVG_INS" "$AVG_QRY"
        done

        # Speedup (QuART vs RAX)
        RAX_INS=$(grep "^${N},${KL},${KL},RAX," "$RESULTS" \
            | cut -d',' -f6 \
            | awk '{s+=$1;c++} END{if(c>0)printf "%.0f",s/c; else print ""}')
        QRT_INS=$(grep "^${N},${KL},${KL},QuART," "$RESULTS" \
            | cut -d',' -f6 \
            | awk '{s+=$1;c++} END{if(c>0)printf "%.0f",s/c; else print ""}')

        if [[ -n "$RAX_INS" && -n "$QRT_INS" && "$QRT_INS" -gt 0 ]]; then
            SPD=$(awk "BEGIN{printf \"%.2fx\", $RAX_INS/$QRT_INS}")
            printf "%-8s | %-10s | %15s |\n" "" "Speedup" "$SPD"
        fi

        printf '%0.s-' {1..60}; echo ""
    done
} > "$SUMMARY"

cat "$SUMMARY"
echo ""
echo "Summary saved to: $SUMMARY"
echo "Logs saved to:    $LOGDIR"
