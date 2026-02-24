#!/bin/bash
# Analyze results from the Airport Operations Redis Benchmark (test_redis_workloads.sh)
#
# Usage:
#   ./analyze_results.sh [results_file.csv]
#
# If no file is given, the most recent airport_redis_test_*.csv in results/ is used.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── find results file ──────────────────────────────────────────────────────
if [[ $# -eq 0 ]]; then
    RESULTS=$(ls -t results/airport_redis_test_*.csv 2>/dev/null | head -1)
    if [[ -z "$RESULTS" ]]; then
        echo "No test results found. Run ./test_redis_workloads.sh first."
        exit 1
    fi
else
    RESULTS="$1"
fi

if [[ ! -f "$RESULTS" ]]; then
    echo "Error: results file not found: $RESULTS"
    exit 1
fi

echo "============================================"
echo "Airport Operations Redis Benchmark Analysis"
echo "============================================"
echo "Results file: $RESULTS"
echo ""
echo "INSERT = schedule load (XADD)  |  QUERY = departure board refresh (XRANGE)"
echo ""

# ── discover tree labels present in the file ───────────────────────────────
LABELS=$(tail -n +2 "$RESULTS" | cut -d',' -f4 | sort -u)

# ── discover K,L pairs ─────────────────────────────────────────────────────
KL_PAIRS=$(tail -n +2 "$RESULTS" | cut -d',' -f2,3 | sort -u -t',' -k1,1n)

# ── condition labels ────────────────────────────────────────────────────
condition_label() {
    case "$1" in
        0)   echo "On-time airline" ;;
        1)   echo "Near-perfect schedule" ;;
        5)   echo "Realistic ops" ;;
        10)  echo "Moderate disruptions" ;;
        25)  echo "Busy hub" ;;
        100) echo "Chaos day" ;;
        *)   echo "K=$1" ;;
    esac
}

# ── header ─────────────────────────────────────────────────────────────────
printf "%-8s | %-10s | %15s | %15s | Condition\n" "K,L" "Tree" "SchedLoad(ns)" "BoardQry(ns)"
printf '%0.s-' {1..80}; echo ""

for KL in $KL_PAIRS; do
    K=$(echo "$KL" | cut -d',' -f1)
    L=$(echo "$KL" | cut -d',' -f2)

    for LABEL in $LABELS; do
            AVG_INS=$(grep "^[^,]*,${K},${L},${LABEL}," "$RESULTS" \
                | cut -d',' -f6 \
                | awk '{s+=$1;c++} END{if(c>0)printf "%.0f",s/c; else print "N/A"}')
            AVG_QRY=$(grep "^[^,]*,${K},${L},${LABEL}," "$RESULTS" \
                | cut -d',' -f7 \
                | awk '{s+=$1;c++} END{if(c>0)printf "%.0f",s/c; else print "N/A"}')

            [[ -z "$AVG_INS" ]] && AVG_INS="N/A"
            [[ -z "$AVG_QRY" ]] && AVG_QRY="N/A"

            printf "%-8s | %-10s | %15s | %15s | %s\n" \
                "$K,$L" "$LABEL" "$AVG_INS" "$AVG_QRY" "$(condition_label "$K")"
        done

    # Speedup: first label vs second label (RAX vs QuART if both present)
        if [[ ${#LABEL_ARR[@]} -ge 2 ]]; then
        A="${LABEL_ARR[0]}"
        B="${LABEL_ARR[1]}"
        A_INS=$(grep "^[^,]*,${K},${L},${A}," "$RESULTS" \
            | cut -d',' -f6 \
            | awk '{s+=$1;c++} END{if(c>0)printf "%.6f",s/c; else print ""}')
        B_INS=$(grep "^[^,]*,${K},${L},${B}," "$RESULTS" \
            | cut -d',' -f6 \
            | awk '{s+=$1;c++} END{if(c>0)printf "%.6f",s/c; else print ""}')

        if [[ -n "$A_INS" && -n "$B_INS" ]]; then
            SPD=$(awk "BEGIN{if($B_INS>0) printf \"%.2fx\", $A_INS/$B_INS; else print \"N/A\"}")
            printf "%-8s | %-10s | %15s |\n" "" "${A}/${B}" "$SPD"
        fi
    fi

    printf '%0.s-' {1..80}; echo ""
done

echo ""
echo "Detailed data:"
tail -n +2 "$RESULTS" | sort -t',' -k2,2n -k4,4 -k5,5n \
    | awk -F',' 'BEGIN{printf "%-6s %-6s %-10s %4s %15s %15s\n","K","L","Tree","Run","Insert(ns)","Query(ns)"}
                 {printf "%-6s %-6s %-10s %4s %15s %15s\n",$2,$3,$4,$5,$6,$7}'
