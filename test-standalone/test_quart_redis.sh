#!/bin/bash
# Test QuART-Redis (rax_quart) with different sortedness levels (K,L values)
# Tests K=L for: 0, 1, 5, 10, 25, 100

# Get script directory and cd to it
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SUFFIX=$(date +"%Y%m%d_%H%M%S")
RESULTSDIR="results"
RESULTS="${RESULTSDIR}/quart_redis_test_${SUFFIX}.csv"
LOGDIR="${RESULTSDIR}/logs"

mkdir -p "$LOGDIR"

# Configuration
N=500000000  # Number of keys in workload files
WORKLOAD_DIR="/home/grad1/cgokmen/bods/workloads/"
REPEAT=3  # Number of repetitions per test

# K,L values to test (where K=L for each test)
KL_VALUES=(0 1 5 10 25 100)

# Test program
TEST_PROG="../src/test-rax-quart-workloads"

echo "=========================================="
echo "QuART-Redis Sortedness Testing"
echo "=========================================="
echo "Testing with N=$N keys"
echo "Repetitions per test: $REPEAT"
echo "K,L values: ${KL_VALUES[@]}"
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

# CSV header
echo "N,K,L,tree_type,run,insert_ns,query_ns" > "$RESULTS"

# Run tests
for KL in "${KL_VALUES[@]}"; do
    K=$KL
    L=$KL
    WORKLOAD="${WORKLOAD_DIR}/workload_N${N}_K${K}_L${L}.bin"
    
    if [ ! -f "$WORKLOAD" ]; then
        echo "WARNING: Workload file not found: $WORKLOAD"
        echo "Skipping K=$K, L=$L"
        continue
    fi
    
    echo "Testing K=$K, L=$L (file: $(basename $WORKLOAD))"
    
    LOGFILE="${LOGDIR}/log_K${K}_L${L}_${SUFFIX}.txt"
    
    for ((i=1; i<=REPEAT; i++)); do
        echo -n "  Run $i/$REPEAT: "
        
        # Run the test
        OUTPUT=$(./$TEST_PROG -f "$WORKLOAD" -N "$N" 2>&1)
        
        # Log everything
        echo "=== Run $i/$REPEAT - $(date) ===" >> "$LOGFILE"
        echo "$OUTPUT" >> "$LOGFILE"
        echo "" >> "$LOGFILE"
        
        # Extract CSV lines (skip header)
        CSV_DATA=$(echo "$OUTPUT" | grep -E "^(RAX|QuART)," | tail -2)
        
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

printf "%-8s | %-10s | %15s | %15s | %15s | %12s\n" "K,L" "Tree" "Insert (ns)" "Query (ns)" "FP Inserts" "Bridges" >> "$SUMMARY"
printf "%.0s-" {1..90} >> "$SUMMARY"
echo "" >> "$SUMMARY"

for KL in "${KL_VALUES[@]}"; do\n" "K,L" "Tree" "Insert (ns)" "Query (ns)" >> "$SUMMARY"
printf "%.0s-" {1..60} >> "$SUMMARY"
echo "" >> "$SUMMARY"

for KL in "${KL_VALUES[@]}"; do
    # RAX results
    RAX_INSERT=$(grep "^$N,$KL,$KL,RAX," "$RESULTS" | cut -d',' -f6 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
    RAX_QUERY=$(grep "^$N,$KL,$KL,RAX," "$RESULTS" | cut -d',' -f7 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
    
    # QuART results
    QUART_INSERT=$(grep "^$N,$KL,$KL,QuART," "$RESULTS" | cut -d',' -f6 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
    QUART_QUERY=$(grep "^$N,$KL,$KL,QuART," "$RESULTS" | cut -d',' -f7 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
    
    printf "%-8s | %-10s | %15s | %15s\n" "$KL,$KL" "RAX" "$RAX_INSERT" "$RAX_QUERY" >> "$SUMMARY"
    printf "%-8s | %-10s | %15s | %15s\n" "" "QuART" "$QUART_INSERT" "$QUART_QUERY
        printf "%-8s | %-10s | %15s |\n" "" "Speedup" "${SPEEDUP}x" >> "$SUMMARY"
    fi
    printf "%.0s-" {1..90} >> "$SUMMARY"
    echo "" >> "$SUMMARY"
done\n" "" "Speedup" "${SPEEDUP}x" >> "$SUMMARY"
    fi
    printf "%.0s-" {1..6$SUMMARY"
echo ""
cat "$SUMMARY"
