# QuART-rax: QuART's stail fast path inside Redis

This fork of Redis (branch `unstable`, forked at b5a37c0e4) is the Redis part of the paper
*QuART: Quick Ingestion to ART through Sortedness Awareness*, Section 5.3 ("Redis").
It adds QuART's **stail** fast path to `rax`, Redis' radix tree, as a wrapper called `raxQuart`
(**QuART-rax** in the paper), uses the wrapper for the tree that indexes a stream and for the
pending-entry lists of consumer groups, and contains the experiments of that section.

The fast path is off by default. `stream-quart-enabled yes` turns it on; everything else is stock Redis.

## Implementation

| Where | What |
|---|---|
| `src/rax_quart.h`, `src/rax_quart.c` | The wrapper. A key is classified by its prefix group, the first `group_depth` bytes, against the key of the last fast-path insert: **FP_INSERT** (same group: insert starting at `fp`), **BRIDGE** (adjacent group: insert from the root and move `fp`), **OTHER** (outlier: insert from the root, keep `fp`; after 16 consecutive outliers `fp` is re-anchored on the current key). `fp` is the depth of a node plus the location of the parent's pointer to it, so a reallocation that rewrites that pointer keeps it valid; an insert that must not move `fp` repairs it when it reallocates the node holding the pointer or splits the compressed node above it. The wrapper also keeps the leaf of the maximum key (`raxQuartTail`, `raxQuartSetTailData`). `group_depth = 3` on 4-byte keys is QuART's stail on 32-bit integers (256-key groups). Stream IDs are 16 bytes and use `group_depth = 7`: the IDs of one 256-millisecond window form a group. |
| `src/rax.h`, `src/rax.c` | `rax->gen`, a generation counter bumped by `raxGenericInsert`, `raxRemove` and defrag, so a wrapper notices a mutation that bypassed it and drops its cached pointers. |
| `src/t_stream.c` | `streamAppendItem` takes the tail listpack from the cached leaf instead of seeking the tree, and replaces its pointer in place instead of a root-down `raxInsert`; every `raxRemove` on the stream tree is followed by `raxQuartInvalidate`. Inserts into the pending-entry lists (XREADGROUP delivery, XCLAIM, XAUTOCLAIM) go through `pelInsert`, a stail wrapper per list created on first use. |
| `src/stream.h`, `src/server.h`, `src/config.c` | Configs `stream-quart-enabled` (default `no`) and `stream-quart-group-depth` (1..15, default 7). |
| `src/debug.c` | `DEBUG STREAM-BULK-INSERT <file> <N>` reads N raw uint32 keys server-side and appends them with `streamAppendItem`, bypassing the client protocol: key `k` becomes entry ID `k-<seq>`, a key smaller than its predecessor starts a new stream, so a sorted file fills one stream. Only the append loop is timed; the reply is `[streams, insert_ns, errors, fp_insert, bridge, other, reset, tail_hit, tail_seek]`. `DEBUG STREAM-QUART-STATS [RESET]` returns the six counters. |
| `src/rdb.c`, `src/object.c`, `src/defrag.c` | Reach the stream's tree through `streamRax(s)` and invalidate the caches when defrag moves nodes. |
| `test-standalone/test_rax_quart_workloads.c` | Standalone harness: inserts a BoDS workload of 32-bit keys, each as 4 big-endian bytes, into a plain `rax` and into a `raxQuart`, then looks every key up. `-c` verifies every key and prints the classification counters. |

## Building

```sh
make -j                               # redis-server and redis-cli; the fast path is a runtime flag
make -C src test-rax-quart-workloads  # standalone harness -> src/test-rax-quart-workloads
```

## Workloads

The experiments read BoDS workload files `workload_N<N>_K<K>_L<L>.bin` (N raw uint32 keys), generated
as in the QuART repository (`experiments/setup.sh`). The scripts take the directory from `WORKLOAD_DIR`
(default `/scratch/cgokmen/bods/workloads`) or a single file from `WORKLOAD`.

## Experiments of Section 5.3

### stail inside Redis Streams (in-server appends)

```sh
cd test-redis && ./ab_bulk_insert.sh 500000000 3 results/inserver.csv off 7
```

Starts a fresh server per run and appends N sorted entries (the K=0 file) to one stream with
`DEBUG STREAM-BULK-INSERT`, with stock `rax` (`off`) and with QuART-rax at group depth 7.
Redis packs 100 entries per tree node, so one tree insert happens per 100 appends; without the fast path
every append still seeks the tail of the tree and re-inserts the node's pointer.
Paper run: `test-redis/results/quart_stail_inserver_N500M_20261016_1604.csv`, median of three:
551.7 ns per entry with `rax`, 512.6 with QuART-rax (1.08x); `tail_hit = N-1`, `tail_seek = 0`;
the tree inserts are 61% FP_INSERT and 39% BRIDGE.

### Sortedness sweep (Figure 17)

```sh
cd test-standalone && ./test_quart_redis.sh            # N=500M, K=L in {0,1,5,10,25,100}, every key verified
python3 plot_redis_performance.py results/<sweep>.csv redis_performance.pdf
```

Each run writes `results/quart_stail_standalone_N500M_<timestamp>.csv` with one row per tree per K
(`verify` = 1 when every key was found; the last four columns are the stail counters).
The paper averages two runs, `quart_stail_standalone_N500M_20261016_1611.csv` and `..._1827.csv`,
concatenated in `quart_stail_standalone_N500M_merged_runs1-2.csv`. The plot script averages the rows of
each (K, tree), draws Figure 17 and prints the ratios: QuART-rax inserts 1.20x faster at K=0, 1.19x at
K=1, 1.16x at K=5, 1.13x at K=10, 1.05x at K=25 and 1.00x at K=100; queries are unchanged.

### Correctness

- Harness: `./src/test-rax-quart-workloads -f <file> -N <n> -c` exits with status 2 on a missing key.
- `test-redis/equivalence_test.sh [N]`: a QuART-enabled and a stock server run the same bulk append,
  XTRIM, XDEL, XADD with explicit IDs, DEBUG RELOAD and XSETID; the md5 of XRANGE must match after every step.
- Redis test suite with the fast path on:
  `./runtest --single unit/type/stream --single unit/type/stream-cgroups --config stream-quart-enabled yes`.
  The five tests of the XREADGROUP ACKED option fail with the fast path off as well.

### Supplementary: consumer-group pending lists (not in the paper)

`test-redis/pel_bench.sh <N> <reps> <csv>` measures server CPU per delivered entry (XREADGROUP) with
stock `rax` and with stail on the pending lists, alone, with XACK of every batch, and with 1% XCLAIMs.
`test-redis/results/quart_stail_pel_N5M_20261016_2159.csv`: 1.03x to 1.05x; a 16-byte pending-list
insert is dominated by allocation, so the saved descent is a small share.

### Superseded

`test-redis/test_redis_workloads.sh` and `test-redis/test_redis_stream_workloads.c` drive XADD over the
client protocol (March 2026) and are not used in the paper. The March standalone results came from an
earlier version of the wrapper and are not committed.
