#!/bin/bash
# Standalone sortedness sweep of the paper (Section 5.3, Figure 17): rax vs QuART-rax (rax + stail
# fast path) on BoDS workloads of 32-bit keys. Every key is inserted, verified, and looked up once.
#
# usage: ./test_quart_redis.sh
#   env  WORKLOAD_DIR  directory of the BoDS files workload_N<N>_K<K>_L<K>.bin
#        WORKLOAD_N    N in the workload file names (default 500000000)
#        N             keys to insert, the first N of each file (default WORKLOAD_N)
#        KS            sortedness levels K=L to run (default "0 1 5 10 25 100")
#        REPS          repetitions of the whole sweep (default 1)
#        OUT           CSV to write (default results/quart_stail_standalone_N<N>_<timestamp>.csv)
# One row per tree per K per repetition:
#   N,K,L,tree_type,insert_ns,query_ns,verify,fp_insert,bridge,other,reset
# tree_type is RAX (stock) or QuART (QuART-rax); verify is 1 when every key was found with its value;
# the last four columns are the stail classification counters of the QuART-rax run.
# CSVs of several sweeps can be concatenated (header once); plot_redis_performance.py averages the
# rows of each (K, tree) and draws Figure 17.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; cd "$SCRIPT_DIR"
WORKLOAD_DIR=${WORKLOAD_DIR:?set WORKLOAD_DIR to the directory of the BoDS workload files}
WORKLOAD_N=${WORKLOAD_N:-500000000}; N=${N:-$WORKLOAD_N}; KS=${KS:-"0 1 5 10 25 100"}; REPS=${REPS:-1}
BIN=../src/test-rax-quart-workloads
SUFFIX=$(date +%Y%m%d_%H%M%S)
OUT=${OUT:-results/quart_stail_standalone_N$((N / 1000000))M_${SUFFIX}.csv}
LOGDIR=results/logs_${SUFFIX}; mkdir -p "$(dirname "$OUT")" "$LOGDIR"
[ -x "$BIN" ] || make -C ../src test-rax-quart-workloads || exit 1

echo "N,K,L,tree_type,insert_ns,query_ns,verify,fp_insert,bridge,other,reset" > "$OUT"
for rep in $(seq 1 "$REPS"); do for K in $KS; do
  F=$WORKLOAD_DIR/workload_N${WORKLOAD_N}_K${K}_L${K}.bin
  [ -f "$F" ] || { echo "missing workload $F" >&2; continue; }
  echo "rep $rep  K=L=$K  ($F)"
  o=$("$BIN" -f "$F" -N "$N" -c -v 2>&1); echo "$o" > "$LOGDIR/K${K}_rep${rep}.log"
  st=$(echo "$o" | grep "QuART: verify" | grep -oE "fp_insert [0-9]+, bridge [0-9]+, other [0-9]+, reset [0-9]+" | tr -d 'a-z_ ')
  echo "$N,$K,$K,RAX,$(echo "$o" | grep '^RAX,' | cut -d, -f2,3),$(echo "$o" | grep -c 'RAX: verify OK'),,,," | tee -a "$OUT"
  echo "$N,$K,$K,QuART,$(echo "$o" | grep '^QuART,' | cut -d, -f2,3),$(echo "$o" | grep -c 'QuART: verify OK'),$st" | tee -a "$OUT"
done; done
echo "results: $OUT   logs: $LOGDIR"
echo "figure:  python3 plot_redis_performance.py $OUT redis_performance.pdf"
