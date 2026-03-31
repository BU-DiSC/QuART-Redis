#!/bin/bash
# Test QuART-Redis (rax_quart) with all workload files in the workload directory

# Get script directory and cd to it
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SUFFIX=$(date +"%Y%m%d_%H%M%S")
RESULTSDIR="results"
RESULTS="${RESULTSDIR}/quart_redis_test_${SUFFIX}.csv"
LOGDIR="${RESULTSDIR}/logs_${SUFFIX}"

mkdir -p "$LOGDIR"

# Configuration
N=500000000  # Number of keys in workload files
WORKLOAD_DIR="/home/grad1/cgokmen/bods/workloads"
REPEAT=3  # Number of repetitions per test

# Test program
TEST_PROG="../src/test-rax-quart-workloads"

echo "=========================================="
echo "QuART-Redis Sortedness Testing"
echo "=========================================="
echo "Testing with N=$N keys"
echo "Repetitions per test: $REPEAT"
echo "Workload directory: $WORKLOAD_DIR"
echo "Results will be saved to: $RESULTS"
echo "=========================================="
echo ""

# Check if test program exists
if [ ! -f "$TEST_PROG" ]; then
    echo "ERROR: Test program not found: $TEST_PROG"
    echo "Building test program..."
    cd ../src && make test-rax-quart-workloads
    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to build test program"
        exit 1
    fi
    cd - > /dev/null
    echo "Build successful!"
    echo ""
fi

# Collect all workload files matching the expected naming pattern
mapfile -t WORKLOAD_FILES < <(ls "${WORKLOAD_DIR}"/workload_N${N}_K*_L*.bin 2>/dev/null | sort)

if [ ${#WORKLOAD_FILES[@]} -eq 0 ]; then
    echo "ERROR: No workload files found in $WORKLOAD_DIR"
    exit 1
fi

echo "Found ${#WORKLOAD_FILES[@]} workload files."
echo ""

# CSV header
echo "N,K,L,tree_type,run,insert_ns,query_ns" > "$RESULTS"

# Run tests over every workload file
for WORKLOAD in "${WORKLOAD_FILES[@]}"; do
    BASENAME=$(basename "$WORKLOAD")
    # Parse K and L from filename: workload_N<N>_K<K>_L<L>.bin
    K=$(echo "$BASENAME" | sed -E 's/workload_N[0-9]+_K([0-9]+)_L([0-9]+)\.bin/\1/')
    L=$(echo "$BASENAME" | sed -E 's/workload_N[0-9]+_K([0-9]+)_L([0-9]+)\.bin/\2/')

    # Only test K=L in the allowed set: 0, 1, 5, 10, 25, 100
    if [ "$K" != "$L" ]; then continue; fi
    case "$K" in
        0|1|5|10|25|100) ;;
        *) continue ;;
    esac

    echo "Testing K=$K, L=$L (file: $BASENAME)"

    LOGFILE="${LOGDIR}/log_K${K}_L${L}_${SUFFIX}.txt"

    for ((i=1; i<=REPEAT; i++)); do
        echo -n "  Run $i/$REPEAT: "

        # Run the test
        OUTPUT=$("$TEST_PROG" -f "$WORKLOAD" -N "$N" 2>&1)

        # Log everything
        echo "=== Run $i/$REPEAT - $(date) ===" >> "$LOGFILE"
        echo "$OUTPUT" >> "$LOGFILE"
        echo "" >> "$LOGFILE"

        # Extract CSV lines
        CSV_DATA=$(echo "$OUTPUT" | grep -E "^(RAX|QuART),")

        if [ -z "$CSV_DATA" ]; then
            echo "FAIL (no output)"
            echo "ERROR: No CSV output received" >> "$LOGFILE"
            continue
        fi

        # Parse and save RAX results
        RAX_LINE=$(echo "$CSV_DATA" | grep "^RAX,")
        if [ -n "$RAX_LINE" ]; then
            INSERT_NS=$(echo "$RAX_LINE" | cut -d',' -f2)
            QUERY_NS=$(echo "$RAX_LINE" | cut -d',' -f3)
            echo "$N,$K,$L,RAX,$i,$INSERT_NS,$QUERY_NS" >> "$RESULTS"
        fi

        # Parse and save QuART results
        QUART_LINE=$(echo "$CSV_DATA" | grep "^QuART,")
        if [ -n "$QUART_LINE" ]; then
            INSERT_NS=$(echo "$QUART_LINE" | cut -d',' -f2)
            QUERY_NS=$(echo "$QUART_LINE" | cut -d',' -f3)
            echo "$N,$K,$L,QuART,$i,$INSERT_NS,$QUERY_NS" >> "$RESULTS"
            echo "OK"
        else
            echo "FAIL"
        fi
    done

    echo ""
done

echo "=========================================="
echo "Testing complete!"
echo "Results saved to: $RESULTS"
echo "Logs saved to: $LOGDIR"
echo "=========================================="

# Generate summary
echo ""
echo "Generating summary..."
SUMMARY="${RESULTSDIR}/quart_redis_summary_${SUFFIX}.txt"

echo "QuART-Redis Sortedness Test Summary" > "$SUMMARY"
echo "Generated: $(date)" >> "$SUMMARY"
echo "=========================================" >> "$SUMMARY"
echo "" >> "$SUMMARY"

printf "%-10s | %-10s | %15s | %15s\n" "K,L" "Tree" "Insert (ns)" "Query (ns)" >> "$SUMMARY"
printf "%.0s-" {1..60} >> "$SUMMARY"
echo "" >> "$SUMMARY"

# Collect unique K,L pairs from results
while IFS=',' read -r rN rK rL rTree rRun rInsert rQuery; do
    echo "$rK,$rL"
done < <(tail -n +2 "$RESULTS") | sort -t',' -k1,1n -k2,2n -u | while IFS=',' read -r K L; do
    RAX_INSERT=$(grep "^$N,$K,$L,RAX," "$RESULTS" | cut -d',' -f6 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
    RAX_QUERY=$(grep "^$N,$K,$L,RAX," "$RESULTS" | cut -d',' -f7 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')

    QUART_INSERT=$(grep "^$N,$K,$L,QuART," "$RESULTS" | cut -d',' -f6 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
    QUART_QUERY=$(grep "^$N,$K,$L,QuART," "$RESULTS" | cut -d',' -f7 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')

    printf "%-10s | %-10s | %15s | %15s\n" "$K,$L" "RAX" "$RAX_INSERT" "$RAX_QUERY" >> "$SUMMARY"
    printf "%-10s | %-10s | %15s | %15s\n" "" "QuART" "$QUART_INSERT" "$QUART_QUERY" >> "$SUMMARY"
    printf "%.0s-" {1..60} >> "$SUMMARY"
    echo "" >> "$SUMMARY"
done

echo ""
cat "$SUMMARY"
