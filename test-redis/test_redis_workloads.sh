#!/bin/bash
# Airport Operations Redis Benchmark
#
# Simulates a real-time airport departure management system backed by Redis
# streams and measures the performance difference between vanilla rax and
# QuART (rax_quart) as Redis's internal stream index.
#
# Each gate at the airport has a dedicated Redis stream (gate:A01, gate:B14 …)
# populated with that gate's flight schedule via XADD, ordered by departure epoch.
# Gate agents and departure boards query their gate streams via XRANGE.
#
#   RAX   – standard Redis build (vanilla rax stream index)
#   QuART – Redis built with USE_QUART=yes (rax_quart fast-path index)
#
# Workload sortedness (K,L) maps directly to airport operating conditions:
#
#   K=0   Perfectly on-time airline: all flights loaded in strict departure
#         order across the day.  QuART’s fast-path fires on every XADD,
#         giving maximum speedup.
#
#   K=1   Near-perfect schedule: rare last-minute gate re-assignments.
#
#   K=5   Realistic ops: occasional equipment swaps and late check-ins
#         cause minor schedule disorder.
#
#   K=10  Moderate disruptions: weather holds, maintenance delays.
#
#   K=25  Busy hub: 25% of flights see gate changes or sequence breaks.
#
#   K=100 Chaos day: ATC ground stop, strikes, mass re-routings — no
#         ordering benefit.  QuART offers no advantage over vanilla rax.
#
# Usage:
#   ./test_redis_workloads.sh [--rax-bin /path/redis-server]
#                             [--quart-bin /path/redis-server-quart]
#                             [--skip-rax] [--skip-quart]
#                             [--port PORT] [--N num_flights]
#
# Results: results/airport_redis_test_<timestamp>.csv

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── default paths ──────────────────────────────────────────────────────────
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_RAX_BIN="$REPO_ROOT/src/redis-server"
DEFAULT_QUART_BIN="$REPO_ROOT/src/redis-server-quart"
TEST_PROG="$REPO_ROOT/src/test-redis-stream-workloads"
WORKLOAD_DIR="$REPO_ROOT/../bods/workloads"  # same workloads as standalone test

# ── configuration ─────────────────────────────────────────────────────────
REDIS_PORT=7379
N=500000000
REPEAT=3
KL_VALUES=(0 1 5 10 25 100)
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
        --port)      REDIS_PORT="$2"; shift 2 ;;
        --N)         N="$2";         shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

SUFFIX=$(date +"%Y%m%d_%H%M%S")
RESULTS="${RESULTSDIR}/airport_redis_test_${SUFFIX}.csv"
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
    echo "WARNING: QuART server binary not found at $QUART_BIN"
    echo "         Build one with:  cd src && make USE_QUART=yes"
    echo "         Then re-run with --quart-bin $QUART_BIN"
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
save ""
appendonly no
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

run_workload_tests() {
    local binary="$1"
    local label="$2"

    echo ""
    echo "=========================================================="
    echo "Testing: $label  ($(basename "$binary"))"
    echo "=========================================================="

    for KL in "${KL_VALUES[@]}"; do
        local K=$KL
        local L=$KL
        local WORKLOAD="${WORKLOAD_DIR}/workload_N${N}_K${K}_L${L}.bin"

        if [[ ! -f "$WORKLOAD" ]]; then
            echo "  WARNING: workload not found: $WORKLOAD — skipping K=$K L=$L"
            continue
        fi

        # Describe the operating condition in airport terms
        case "$KL" in
            0)   CONDITION="Perfectly on-time airline (K=0)" ;;
            1)   CONDITION="Near-perfect schedule, rare re-assignments (K=1)" ;;
            5)   CONDITION="Realistic ops: occasional equipment swaps (K=5)" ;;
            10)  CONDITION="Moderate disruptions: weather/maintenance (K=10)" ;;
            25)  CONDITION="Busy hub: frequent gate changes (K=25)" ;;
            100) CONDITION="Chaos day: ATC ground stop, mass re-routings (K=100)" ;;
            *)   CONDITION="K=$KL" ;;
        esac

        echo "  Scenario: $CONDITION"
        echo "  Workload: $(basename "$WORKLOAD")"
        local LOGFILE="${LOGDIR}/log_${label}_K${K}_L${L}_${SUFFIX}.txt"

        for ((run=1; run<=REPEAT; run++)); do
            printf "    Run %d/%d: " "$run" "$REPEAT"

            # Start a fresh Redis instance for each run to avoid state
            start_redis "$binary" || { echo "FAIL (server start)"; continue; }

            local OUTPUT
            OUTPUT=$("$TEST_PROG" \
                -f "$WORKLOAD" \
                -N "$N" \
                -h 127.0.0.1 \
                -p "$REDIS_PORT" \
                -n "$label" 2>&1) || true

            stop_redis

            # Log everything
            {
                echo "=== $label K=$K L=$L Run $run/$REPEAT — $(date) ==="
                echo "=== Scenario: $CONDITION ==="
                echo "$OUTPUT"
                echo ""
            } >> "$LOGFILE"

            # Extract CSV line (skip the header line)
            local LINE
            LINE=$(echo "$OUTPUT" | grep "^${label},")

            if [[ -z "$LINE" ]]; then
                echo "FAIL (no CSV output)"
                echo "--- raw output ---" >> "$LOGFILE"
                echo "$OUTPUT" >> "$LOGFILE"
                continue
            fi

            local INSERT_NS QUERY_NS
            INSERT_NS=$(echo "$LINE" | cut -d',' -f2)
            QUERY_NS=$(echo "$LINE"  | cut -d',' -f3)

            echo "$N,$K,$L,$label,$run,$INSERT_NS,$QUERY_NS" >> "$RESULTS"
            echo "OK  (schedule_load=${INSERT_NS}ns  board_query=${QUERY_NS}ns)"
        done
        echo ""
    done
}

# ── CSV header ─────────────────────────────────────────────────────────────
echo "N,K,L,tree_type,run,insert_ns,query_ns" > "$RESULTS"

# ── banner ─────────────────────────────────────────────────────────────────
echo "=========================================================="
echo "Airport Operations Redis Benchmark"
echo "=========================================================="
echo "Simulating N=$N flight events  |  REPEAT=$REPEAT  |  PORT=$REDIS_PORT"
echo ""
echo "Operating conditions tested (K,L):"
echo "  K=0   Perfectly on-time airline: full-day schedule in departure order"
echo "  K=1   Near-perfect: rare last-minute gate re-assignments"
echo "  K=5   Realistic ops: occasional equipment swaps and late check-ins"
echo "  K=10  Moderate disruptions: weather holds, maintenance delays"
echo "  K=25  Busy hub: frequent gate changes and sequence breaks"
echo "  K=100 Chaos day: ATC ground stop, mass re-routings"
echo ""
echo "Results: $RESULTS"
echo "=========================================================="

# ── run tests ──────────────────────────────────────────────────────────────
[[ $SKIP_RAX   -eq 0 ]] && run_workload_tests "$RAX_BIN"   "RAX"
[[ $SKIP_QUART -eq 0 ]] && run_workload_tests "$QUART_BIN" "QuART"

echo "=========================================================="
echo "Done!  Results: $RESULTS"
echo "=========================================================="

# ── summary ────────────────────────────────────────────────────────────────
echo ""
echo "Generating summary..."
SUMMARY="${RESULTSDIR}/airport_redis_summary_${SUFFIX}.txt"

{
    echo "Airport Operations Redis Benchmark Summary"
    echo "Generated: $(date)"
    echo "N=$N flights simulated  |  REPEAT=$REPEAT  |  PORT=$REDIS_PORT"
    echo "Streams: gate:{A-E}{01-30} — INSERT=XADD, QUERY=XRANGE (departure board)"
    echo "========================================================"
    printf "%-8s | %-10s | %15s | %15s | Condition\n" "K,L" "Tree" "SchedLoad(ns)" "BoardQry(ns)"
    printf '%0.s-' {1..80}; echo ""

    for KL in "${KL_VALUES[@]}"; do
        case "$KL" in
            0)   COND="On-time (K=0)" ;;
            1)   COND="Near-perfect (K=1)" ;;
            5)   COND="Realistic ops (K=5)" ;;
            10)  COND="Moderate disruptions (K=10)" ;;
            25)  COND="Busy hub (K=25)" ;;
            100) COND="Chaos day (K=100)" ;;
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

            printf "%-8s | %-10s | %15s | %15s | %s\n" \
                "$KL,$KL" "$LABEL" "$AVG_INS" "$AVG_QRY" "$COND"
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

        printf '%0.s-' {1..80}; echo ""
    done
} > "$SUMMARY"

cat "$SUMMARY"
echo ""
echo "Summary saved to: $SUMMARY"
echo "Logs saved to:    $LOGDIR"
