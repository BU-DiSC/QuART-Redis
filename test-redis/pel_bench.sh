#!/bin/bash
# Consumer-group pending-list benchmark: server CPU per delivered entry (XREADGROUP) with stock rax
# vs QuART stail on the pending lists. Variants: deliver (ascending deliveries only), ack (XACK of
# each batch after delivery), claim (1% of inserts are older IDs re-claimed from another consumer).
# usage: pel_bench.sh <N entries> <reps> <csv>     (needs ../src/redis-server, ../src/redis-cli)
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/../src" && pwd); W=${WORKLOAD:-${WORKLOAD_DIR:?set WORKLOAD_DIR or WORKLOAD}/workload_N500000000_K0_L0.bin}
TMP=${WORK:-/tmp/quart-redis-$USER}; mkdir -p $TMP; N=$1; REPS=$2; CSV=$3; B=10000; PORT=${PORT:-7430}
mkfifo $TMP/nap9.fifo 2>/dev/null; nap() { read -t "$1" <> $TMP/nap9.fifo; }
cli() { $R/redis-cli -p $PORT "$@"; }
cpu() { cli INFO cpu | awk -F: '/^used_cpu_(sys|user):/ {s+=$2} END {printf "%.6f", s}'; }
[ -f "$CSV" ] || echo "N,config,variant,rep,server_cpu_ns_per_entry,wall_ms,fp_insert,bridge,other,reset,pending_c1,pending_c2" > "$CSV"
for rep in $(seq 1 $REPS); do for cfg in off on; do for variant in deliver ack claim; do
  rm -f $TMP/dump.rdb
  $R/redis-server --port $PORT --bind 127.0.0.1 --save "" --appendonly no --enable-debug-command yes \
     --stream-quart-enabled $( [ $cfg = on ] && echo yes || echo no ) --daemonize no --logfile $TMP/pel_srv.log --dir $TMP > /dev/null 2>&1 &
  for i in $(seq 1 120); do cli PING > /dev/null 2>&1 && break; nap 0.5; done
  cli DEBUG STREAM-BULK-INSERT $W $N > /dev/null; cli XGROUP CREATE stream:0 g1 0 > /dev/null
  nb=$((N / B)); start=1
  if [ $variant = claim ]; then   # pre-deliver the first fifth to c2; those IDs get claimed into c1 later
    for b in $(seq 1 $((nb/5))); do cli XREADGROUP GROUP g1 c2 COUNT $B STREAMS stream:0 '>' > /dev/null; done; start=$((nb/5 + 1))
  fi
  cli DEBUG STREAM-QUART-STATS RESET > /dev/null; c0=$(cpu); t0=$(date +%s%N)
  for b in $(seq $start $nb); do
    cli XREADGROUP GROUP g1 c1 COUNT $B STREAMS stream:0 '>' > /dev/null
    case $variant in
      ack)   s=$(( (b-1)*B + 1 )); e=$(( b*B )); printf "XACK stream:0 g1 %s\n" "$(seq -f '%g-0' $s $e | tr '\n' ' ')" | cli > /dev/null ;;
      claim) j=$((b - start)); s=$(( j*100 + 1 )); e=$(( j*100 + 100 )); printf "XCLAIM stream:0 g1 c1 0 %s JUSTID\n" "$(seq -f '%g-0' $s $e | tr '\n' ' ')" | cli > /dev/null ;;
    esac
  done
  t1=$(date +%s%N); c1=$(cpu); st=$(cli DEBUG STREAM-QUART-STATS | tr '\n' ' '); set -- $st
  delivered=$(( (nb - start + 1) * B ))
  p1=$(cli XPENDING stream:0 g1 | sed -n '4,$p' | tr '\n' ' '); 
  printf "%s,%s,%s,%s,%.1f,%d,%s,%s,%s,%s,%s\n" "$N" "$cfg" "$variant" "$rep" "$(echo "($c1 - $c0) * 1000000000 / $delivered" | bc -l)" "$(( (t1-t0)/1000000 ))" "$1" "$2" "$3" "$4" "$(echo $p1 | tr ' ' ';')" | tee -a "$CSV"
  cli SHUTDOWN NOSAVE > /dev/null 2>&1; nap 1
done; done; done
