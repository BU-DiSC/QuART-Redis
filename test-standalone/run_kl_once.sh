#!/bin/bash
# Run one pass of QuART vs RAX for K=L values: 0, 1, 5, 10, 25, 100
# Usage: ./run_kl_once.sh [N]
#   N: number of keys to read from each workload file (default: 500000000)
#      Set smaller (e.g. 10000000) for a quick smoke-test.

N=${1:-500000000}
FILE_N=500000000      # workload files are always created with N=500000000
WORKLOAD_DIR="/home/grad1/cgokmen/bods/workloads"
TEST_PROG="$(dirname "$0")/../src/test-rax-quart-workloads"
KL_VALUES=(0 1 5 10 25 100)

echo "K,L,insert_rax_ns,insert_quart_ns,insert_speedup,fp_inserts,regular_inserts,bridges,resets"

for KL in "${KL_VALUES[@]}"; do
    WORKLOAD="${WORKLOAD_DIR}/workload_N${FILE_N}_K${KL}_L${KL}.bin"
    if [ ! -f "$WORKLOAD" ]; then
        echo "${KL},${KL},N/A,N/A,N/A,N/A,N/A,N/A,N/A  # missing: $WORKLOAD" >&2
        continue
    fi

    OUT=$("$TEST_PROG" -f "$WORKLOAD" -N "$N" -v 2>&1)
    if [ $? -ne 0 ]; then
        echo "${KL},${KL},FAIL,FAIL,FAIL,FAIL,FAIL,FAIL,FAIL" >&2
        echo "--- raw output for K=${KL} ---" >&2
        echo "$OUT" >&2
        continue
    fi

    RAX_INS=$(echo   "$OUT" | grep "^  RAX: Inserted"   | awk '{print $6}')
    QUART_INS=$(echo "$OUT" | grep "^  QuART: Inserted" | awk '{print $6}')
    FP=$(echo        "$OUT" | grep "QuART Stats:"       | grep -oP 'FP=\K[0-9]+')
    REG=$(echo       "$OUT" | grep "QuART Stats:"       | grep -oP 'Regular=\K[0-9]+')
    BR=$(echo        "$OUT" | grep "QuART Stats:"       | grep -oP 'Bridges=\K[0-9]+')
    RS=$(echo        "$OUT" | grep "QuART Stats:"       | grep -oP 'Resets=\K[0-9]+')

    if [ -n "$RAX_INS" ] && [ -n "$QUART_INS" ] && [ "$QUART_INS" -gt 0 ] 2>/dev/null; then
        SPEEDUP=$(awk "BEGIN{printf \"%.3f\", $RAX_INS/$QUART_INS}")
    else
        SPEEDUP="N/A"
    fi

    echo "${KL},${KL},${RAX_INS},${QUART_INS},${SPEEDUP},${FP},${REG},${BR},${RS}"
done
