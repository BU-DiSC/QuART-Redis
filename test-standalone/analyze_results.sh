#!/bin/bash
# Quick analysis script for QuART-Redis test results

# Get script directory and cd to it
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ $# -eq 0 ]; then
    # Find the most recent results file
    RESULTS=$(ls -t results/quart_redis_test_*.csv 2>/dev/null | head -1)
    if [ -z "$RESULTS" ]; then
        echo "No test results found. Run ./test_quart_redis.sh first."
        exit 1
    fi
else
    RESULTS=$1
fi

if [ ! -f "$RESULTS" ]; then
    echo "Error: Results file not found: $RESULTS"
    exit 1
fi

echo "=========================================="
echo "QuART-Redis Test Analysis"
echo "=========================================="
echo "Results file: $RESULTS"
echo ""

echo "Performance Summary (Average times in nanoseconds)"
echo "---------------------------------------------------"
echo ""

printf "%-8s | %-10s | %15s | %15s\n" "K,L" "Tree" "Insert (ns)" "Query (ns)"
printf "%.0s-" {1..60}
echo ""

# Get unique K,L pairs
KL_PAIRS=$(tail -n +2 "$RESULTS" | cut -d',' -f2,3 | sort -u -t',' -k1,1n)

for KL in $KL_PAIRS; do
    K=$(echo $KL | cut -d',' -f1)
    L=$(echo $KL | cut -d',' -f2)
    
    # RAX stats
    RAX_INSERT=$(grep "^[^,]*,$K,$L,RAX," "$RESULTS" | cut -d',' -f6 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
    RAX_QUERY=$(grep "^[^,]*,$K,$L,RAX," "$RESULTS" | cut -d',' -f7 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
    
    # QuART stats
    QUART_INSERT=$(grep "^[^,]*,$K,$L,QuART," "$RESULTS" | cut -d',' -f6 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
    QUART_QUERY=$(grep "^[^,]*,$K,$L,QuART," "$RESULTS" | cut -d',' -f7 | \
        awk '{sum+=$1; count++} END {if(count>0) printf "%.0f", sum/count; else print "N/A"}')
    
    printf "%-8s | %-10s | %15s | %15s\n" "$K,$L" "RAX" "$RAX_INSERT" "$RAX_QUERY"
    printf "%-8s | %-10s | %15s | %15s\n" "" "QuART" "$QUART_INSERT" "$QUART_QUERY"
    printf "%.0s-" {1..60}
    echo ""
done

echo ""
echo "Speedup Analysis (QuART vs RAX)"
echo "---------------------------------------------------"
echo ""

printf "%-8s | %15s | %15s | %15s\n" "K,L" "Insert Speedup" "Query Speedup" "FP Usage %"
printf "%.0s-" {1..70}
echo ""

for KL in $KL_PAIRS; do\n" "K,L" "Insert Speedup" "Query Speedup"
printf "%.0s-" {1..5ut -d',' -f1)
    L=$(echo $KL | cut -d',' -f2)
    
    # Get averages
    RAX_INSERT=$(grep "^[^,]*,$K,$L,RAX," "$RESULTS" | cut -d',' -f6 | \
        awk '{sum+=$1; count++} END {if(count>0) print sum/count; else print "0"}')
    RAX_QUERY=$(grep "^[^,]*,$K,$L,RAX," "$RESULTS" | cut -d',' -f7 | \
        awk '{sum+=$1; count++} END {if(count>0) print sum/count; else print "0"}')
    
    QUART_INSERT=$(grep "^[^,]*,$K,$L,QuART," "$RESULTS" | cut -d',' -f6 | \
        awk '{sum+=$1; count++} END {if(count>0) print sum/count; else print "0"}')
    QUART_QUERY=$(grep "^[^,]*,$K,$L,QuART," "$RESULTS" | cut -d',' -f7 | \
        awk '{sum+=$1; count++} END {if(count>0) print sum/count; else print "0"}')
    
    # Calculate speedup
    if command -v bc &> /dev/null; then
        if [ "$QUART_INSERT" != "0" ]; then
            INSERT_SPEEDUP=$(echo "scale=2; $RAX_INSERT / $QUART_INSERT" | bc)        
        else
            INSERT_SPEEDUP="N/A"
        fi
        
        if [ "$QUART_QUERY" != "0" ]; then
            QUERY_SPEEDUP=$(echo "scale=2; $RAX_QUERY / $QUART_QUERY" | bc)
        else
            QUERY_SPEEDUP="N/A"
        fi
    else
        # Fallback to awk if bc not available
        INSERT_SPEEDUP=$(awk -v r="$RAX_INSERT" -v q="$QUART_INSERT" 'BEGIN {if(q!=0) printf "%.2f", r/q; else print "N/A"}')
        QUERY_SPEEDUP=$(awk -v r="$RAX_QUERY" -v q="$QUART_QUERY" 'BEGIN {if(q!=0) printf "%.2f", r/q; else print "N/A"}')
    fi
    
    printf "%-8s | %15s | %15s\n" "$K,$L" "${INSERT_SPEEDUP}x" "${QUERY_SPEEDUP}x"
done

printf "%.0s-" {1..70}
echo ""

echo ""
echo "Notes:"
echo "  - Speedup > 1.0x means QuART is faster than RAX"
echo "  - Speedup < 1.0x means QuART is slower than RAX"
echo "  - FP Usage % shows what percentage of insertions used fast-path"
echo "  - Times are averaged across all repetitions"
50}
echo ""

echo ""
echo "Notes:"
echo "  - Speedup > 1.0x means QuART is faster than RAX"
echo "  - Speedup < 1.0x means QuART is slower than RAX