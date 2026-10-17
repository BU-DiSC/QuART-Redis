#!/bin/bash
# In-server A/B: fresh server per run; DEBUG STREAM-BULK-INSERT of N sorted entries into one stream.
# Example: ./ab_bulk_insert.sh 500000000 3 results/inserver.csv off 7     (N, repetitions, CSV, then configs: "off" = stock rax, a number = QuART group depth)
# Needs ../src/redis-server and ../src/redis-cli; WORK (default /tmp/quart-redis-$USER) holds logs; delete its dump.rdb between runs.
# WORKLOAD (or WORKLOAD_DIR) selects the BoDS file whose uint32 keys become the stream IDs (default: the N=500M K=0 file).
# usage: ab_bulk_insert.sh <N> <reps> <csv> [D values...]   (D = quart group depth; "off" = stock rax)
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/../src" && pwd); W=${WORKLOAD:-${WORKLOAD_DIR:-/scratch/cgokmen/bods/workloads}/workload_N500000000_K0_L0.bin}
TMP=${WORK:-/tmp/quart-redis-$USER}; mkdir -p $TMP; N=$1; REPS=$2; CSV=$3; shift 3; CONFIGS="$@"; PORT=${PORT:-7410}
mkfifo $TMP/nap4.fifo 2>/dev/null; nap() { read -t "$1" <> $TMP/nap4.fifo; }
[ -f "$CSV" ] || echo "N,config,rep,insert_ns,ns_per_entry,fp_insert,bridge,other,reset,tail_hit,tail_seek" > "$CSV"
for rep in $(seq 1 $REPS); do for cfg in $CONFIGS; do
  rm -f $TMP/dump.rdb
  if [ "$cfg" = "off" ]; then opts="--stream-quart-enabled no"; else opts="--stream-quart-enabled yes --stream-quart-group-depth $cfg"; fi
  $R/redis-server --port $PORT --bind 127.0.0.1 --save "" --appendonly no --enable-debug-command yes $opts \
     --daemonize no --logfile $TMP/ab_srv.log --dir $TMP > /dev/null 2>&1 &
  for i in $(seq 1 120); do $R/redis-cli -p $PORT PING > /dev/null 2>&1 && break; nap 0.5; done
  out=$($R/redis-cli -p $PORT DEBUG STREAM-BULK-INSERT $W $N | tr '\n' ' ')
  set -- $out; ns=$2; printf "%s,%s,%s,%s,%.1f,%s,%s,%s,%s,%s,%s\n" "$N" "$cfg" "$rep" "$ns" "$(echo "$ns / $N" | bc -l)" "$4" "$5" "$6" "$7" "$8" "$9" | tee -a "$CSV"
  $R/redis-cli -p $PORT SHUTDOWN NOSAVE > /dev/null 2>&1; nap 1
done; done
