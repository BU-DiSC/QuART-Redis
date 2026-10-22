#!/bin/bash
# Equivalence test: a QuART-enabled server and a stock server get the same operations; outputs must match.
# usage: equivalence_test.sh [N]   (needs ../src/redis-server, ../src/redis-cli; WORKLOAD or WORKLOAD_DIR picks the BoDS file)
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/../src" && pwd); W=${WORKLOAD:-${WORKLOAD_DIR:?set WORKLOAD_DIR or WORKLOAD}/workload_N500000000_K0_L0.bin}
TMP=${WORK:-/tmp/quart-redis-$USER}; mkdir -p $TMP; N=${1:-1000000}
rm -f $TMP/dump.rdb; rm -rf $TMP/appendonlydir; mkfifo $TMP/nap3.fifo 2>/dev/null; nap() { read -t "$1" <> $TMP/nap3.fifo; }
start() { # $1=port $2=quart yes/no
  $R/redis-server --port $1 --bind 127.0.0.1 --save "" --appendonly no --enable-debug-command yes \
     --stream-quart-enabled $2 --daemonize no --logfile $TMP/equiv_$1.log --dir $TMP > /dev/null 2>&1 &
  for i in $(seq 1 60); do $R/redis-cli -p $1 PING > /dev/null 2>&1 && break; nap 0.5; done
}
cli() { $R/redis-cli -p "$@"; }
dump() { cli $1 XRANGE stream:0 - + COUNT 100000000 | md5sum | cut -c1-12; }
start 7401 yes; start 7402 no
for P in 7401 7402; do
  echo "--- port $P (stream-quart-enabled $(cli $P CONFIG GET stream-quart-enabled | tail -1))"
  echo "bulk: $(cli $P DEBUG STREAM-BULK-INSERT $W $N | tr '\n' ' ')"
  echo "xlen=$(cli $P XLEN stream:0) entries-added=$(cli $P XINFO STREAM stream:0 | grep -A1 entries-added | tail -1) last=$(cli $P XINFO STREAM stream:0 | grep -A1 last-generated-id | tail -1)"
  echo "md5 after bulk: $(dump $P)"
  # mutations: trim the head, delete the last 120 entries (crosses a node boundary), append with explicit IDs
  cli $P XTRIM stream:0 MAXLEN 50000 > /dev/null
  ids=$(cli $P XREVRANGE stream:0 + - COUNT 120 | grep -oE '^[0-9]+\) [0-9]+\) "[0-9]+-[0-9]+"' | grep -oE '[0-9]+-[0-9]+')
  cli $P XDEL stream:0 $ids > /dev/null
  for i in $(seq 1 250); do echo "XADD stream:0 $((2000000000+i))-0 v $i"; done | cli $P > /dev/null
  echo "md5 after trim/del/add: $(dump $P) xlen=$(cli $P XLEN stream:0)"
  cli $P DEBUG RELOAD > /dev/null
  echo "md5 after reload: $(dump $P)"
  for i in $(seq 1 300); do echo "XADD stream:0 $((2100000000+i))-0 v $i"; done | cli $P > /dev/null
  cli $P XSETID stream:0 2100000300-0 > /dev/null
  for i in $(seq 1 50); do echo "XADD stream:0 $((2200000000+i))-5 v $i"; done | cli $P > /dev/null
  echo "md5 after reload+add: $(dump $P) xlen=$(cli $P XLEN stream:0) first=$(cli $P XINFO STREAM stream:0 | grep -A1 first-entry | tail -1 | head -c 40)"
  cli $P SHUTDOWN NOSAVE > /dev/null 2>&1
done
