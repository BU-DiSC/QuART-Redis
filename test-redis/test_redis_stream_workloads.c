/* Airport Operations Redis Benchmark
 *
 * Models a real-time airport operations system backed by Redis streams.
 * Each gate at the airport has a dedicated Redis stream (gate:A01, gate:B14 …)
 * containing that gate's flight departure schedule, ordered by scheduled
 * departure time.
 *
 * INSERT phase — "Dispatch Center Loads Flight Schedule"
 *   The operations center pushes every flight event into its gate's stream via
 *   XADD.  The uint32 workload keys are treated as scheduled departure epochs
 *   (seconds since midnight on the day's base).  Within each gate the schedule
 *   is always monotonically increasing; when the key sequence breaks (a rax
 *   "bridge") the flight is reassigned to the next gate.
 *
 *   K=0  (sorted)  → one gate sees the entire day in order — QuART fast-path
 *                     fires on every insertion.
 *   K=100 (random) → constant gate re-assignments — no fast-path benefit.
 *
 * QUERY phase — "Gate Agent / Departure Board Refresh"
 *   Every active gate agent queries their gate's full flight list via XRANGE,
 *   simulating a departure board polling for schedule updates.
 *
 * Stream key:  gate:{terminal}{gate_num}   e.g. gate:A07, gate:C23
 * Stream ID:   {departure_epoch}-{dup_seq}
 * Fields:      flight_id, airline, dest, status, terminal, dep_epoch
 *
 * Usage:
 *   ./test-redis-airport -f <workload_file> -N <num_keys>
 *                        [-h <host>] [-p <port>] [-a <auth>]
 *                        [-n <name>] [-Q <query_pct>] [-v]
 *
 * CSV output:
 *   tree_type,insert_ns,query_ns
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>

#include "../deps/hiredis/hiredis.h"
#include "../src/zmalloc.h"

/* Max number of commands queued in hiredis's write buffer at one time.
 * Flushing replies every PIPELINE_BATCH commands keeps memory bounded
 * regardless of N. 8192 gives good throughput without large buffers. */
#define PIPELINE_BATCH 8192

/* ── airport reference data ─────────────────────────────────────────────── */

static const char *AIRLINES[] = {
    "AA", "UA", "DL", "WN", "AS",
    "B6", "NK", "F9", "G4", "SY",
    "HA", "MX", "VX", "9E", "OO"
};
#define NUM_AIRLINES 15

static const char *DESTINATIONS[] = {
    "LAX", "JFK", "ORD", "ATL", "DFW",
    "DEN", "SFO", "SEA", "MIA", "BOS",
    "LAS", "PHX", "CLT", "MSP", "DTW",
    "PHL", "LGA", "IAH", "EWR", "MCO"
};
#define NUM_DESTINATIONS 20

/* Terminal letters and gate-number ranges per terminal */
static const char TERMINAL_LETTERS[] = { 'A', 'B', 'C', 'D', 'E' };
#define NUM_TERMINALS 5
#define GATES_PER_TERMINAL 30   /* gates 01-30 per terminal letter */

/* Build gate code string from a gate index (0-based). */
static void gate_code(size_t gate_idx, char *buf, size_t buflen) {
    char letter = TERMINAL_LETTERS[gate_idx % NUM_TERMINALS];
    unsigned int num = (unsigned int)((gate_idx / NUM_TERMINALS) % GATES_PER_TERMINAL) + 1;
    snprintf(buf, buflen, "%c%02u", letter, num);
}

/* Derive flight ID string from the raw key integer. */
static void flight_id(uint32_t key, char *buf, size_t buflen) {
    const char *airline = AIRLINES[key % NUM_AIRLINES];
    unsigned int fnum = (key / NUM_AIRLINES) % 9000 + 1000;
    snprintf(buf, buflen, "%s%04u", airline, fnum);
}

/* Derive destination from the raw key integer. */
static const char *destination(uint32_t key) {
    return DESTINATIONS[key % NUM_DESTINATIONS];
}

/* ~14% of flights are delayed (divisible by 7), the rest on schedule. */
static const char *flight_status(uint32_t key) {
    return (key % 7 == 0) ? "DLY" : "SCH";
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static long long get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static uint32_t *read_workload(const char *filename, size_t num_keys) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open workload file %s\n", filename);
        return NULL;
    }
    uint32_t *keys = zmalloc(num_keys * sizeof(uint32_t));
    if (!keys) {
        fprintf(stderr, "Error: cannot allocate memory for %zu keys\n", num_keys);
        fclose(fp);
        return NULL;
    }
    size_t n = fread(keys, sizeof(uint32_t), num_keys, fp);
    fclose(fp);
    if (n != num_keys)
        fprintf(stderr, "Warning: read %zu of %zu keys\n", n, num_keys);
    return keys;
}

/* Delete all gate streams created during the test (batched pipeline). */
static void cleanup_gates(redisContext *ctx, size_t num_gates, int verbose) {
    char code[8];
    size_t errors = 0;
    size_t sent = 0;

    for (size_t i = 0; i < num_gates; i++) {
        gate_code(i, code, sizeof(code));
        redisAppendCommand(ctx, "DEL gate:%s", code);
        sent++;

        if (sent == PIPELINE_BATCH || i == num_gates - 1) {
            for (size_t j = 0; j < sent; j++) {
                redisReply *r;
                redisGetReply(ctx, (void **)&r);
                if (r) freeReplyObject(r);
                else errors++;
            }
            sent = 0;
        }
    }
    if (verbose && errors)
        fprintf(stderr, "  Warning: %zu DEL errors during gate cleanup\n", errors);
}

/* ── INSERT: dispatch center loads the day's flight schedule ─────────────── */
/*
 * Each uint32_t key is treated as a departure epoch.  Within each gate stream
 * the schedule must be strictly increasing; a break (key < last_key) triggers
 * a gate assignment switch — the same way a rax tree sees a "bridge".
 *
 * Entry fields: flight_id, airline, dest, status, terminal, dep_epoch
 */
static long long run_insert(redisContext *ctx, uint32_t *keys, size_t num_keys,
                            int verbose, size_t *out_gates) {
    size_t gate_idx = 0;
    uint32_t last_key = 0;
    int has_last = 0;
    uint32_t dup_seq = 0;

    char gate[8], fid[12];
    size_t queued = 0;  /* commands in the current pipeline batch */
    size_t errors = 0;

    if (verbose)
        printf("  Loading %zu flight events in batches of %d...\n",
               num_keys, PIPELINE_BATCH);

    long long start = get_time_ns();

    for (size_t i = 0; i < num_keys; i++) {
        uint32_t k = keys[i];

        if (has_last) {
            if (k < last_key) {
                gate_idx++;
                dup_seq = 0;
            } else if (k == last_key) {
                dup_seq++;
            } else {
                dup_seq = 0;
            }
        }

        gate_code(gate_idx, gate, sizeof(gate));
        flight_id(k, fid, sizeof(fid));

        redisAppendCommand(ctx,
            "XADD gate:%s %u-%u "
            "flight_id %s "
            "airline %s "
            "dest %s "
            "status %s "
            "terminal %c "
            "dep_epoch %u",
            gate, k, dup_seq,
            fid,
            AIRLINES[k % NUM_AIRLINES],
            destination(k),
            flight_status(k),
            TERMINAL_LETTERS[gate_idx % NUM_TERMINALS],
            k);
        queued++;

        /* Flush a batch: read all pending replies before queuing more */
        if (queued == PIPELINE_BATCH || i == num_keys - 1) {
            for (size_t j = 0; j < queued; j++) {
                redisReply *r;
                if (redisGetReply(ctx, (void **)&r) != REDIS_OK) { errors++; continue; }
                if (!r || r->type == REDIS_REPLY_ERROR)           { errors++; }
                if (r) freeReplyObject(r);
            }
            queued = 0;
        }

        last_key = k;
        has_last = 1;
    }

    long long end = get_time_ns();
    *out_gates = gate_idx + 1;

    if (errors && verbose)
        fprintf(stderr, "  Warning: %zu XADD errors\n", errors);

    return end - start;
}

/* ── QUERY: departure board / gate agent refresh ─────────────────────────── */
/*
 * Each query is an XRANGE over one gate's complete flight list, as a gate
 * agent or departure display would scan all upcoming departures.
 */
static long long run_query(redisContext *ctx, size_t num_gates,
                           size_t query_count, int verbose) {
    if (query_count > num_gates) query_count = num_gates;
    if (query_count == 0) query_count = 1;

    size_t stride = num_gates / query_count;
    if (stride == 0) stride = 1;

    char code[8];
    size_t errors = 0;
    size_t queued = 0;

    if (verbose)
        printf("  Querying %zu gate departure boards (XRANGE) in batches of %d...\n",
               query_count, PIPELINE_BATCH);

    long long start = get_time_ns();

    for (size_t i = 0; i < query_count; i++) {
        size_t idx = i * stride;
        if (idx >= num_gates) idx = num_gates - 1;
        gate_code(idx, code, sizeof(code));
        redisAppendCommand(ctx, "XRANGE gate:%s - +", code);
        queued++;

        if (queued == PIPELINE_BATCH || i == query_count - 1) {
            for (size_t j = 0; j < queued; j++) {
                redisReply *r;
                if (redisGetReply(ctx, (void **)&r) != REDIS_OK) { errors++; continue; }
                if (!r || r->type == REDIS_REPLY_ERROR)           { errors++; }
                if (r) freeReplyObject(r);
            }
            queued = 0;
        }
    }

    long long end = get_time_ns();

    if (errors && verbose)
        fprintf(stderr, "  Warning: %zu XRANGE errors\n", errors);

    return end - start;
}

/* ── usage ───────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    printf("Airport Operations Redis Benchmark\n");
    printf("===================================\n");
    printf("Simulates a real-time airport departure management system backed by\n");
    printf("Redis streams.  Each gate has a stream (gate:A01, gate:B14 …) holding\n");
    printf("its flight schedule ordered by departure epoch.  Low K = sorted\n");
    printf("schedule (QuART fast-path); high K = chaotic re-assignments (no gain).\n\n");
    printf("Usage: %s -f <workload_file> -N <num_keys>\n", prog);
    printf("            [-h <host>] [-p <port>] [-a <auth>]\n");
    printf("            [-n <name>] [-Q <query_pct>] [-v]\n\n");
    printf("Options:\n");
    printf("  -f <file>    Binary workload file (uint32_t array, required)\n");
    printf("  -N <n>       Number of flight events to simulate (default: 500000000)\n");
    printf("  -h <host>    Redis host (default: 127.0.0.1)\n");
    printf("  -p <port>    Redis port (default: 6379)\n");
    printf("  -a <pass>    Redis AUTH password (optional)\n");
    printf("  -n <name>    Label for CSV tree_type column (default: REDIS)\n");
    printf("  -Q <pct>     Percentage of gates to query (1-100, default: 100)\n");
    printf("  -v           Verbose output\n\n");
    printf("CSV output: tree_type,insert_ns,query_ns\n");
}

/* ----------------------------- main --------------------------------------- */

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 6379;
    const char *auth = NULL;
    const char *name = "REDIS";
    char *workload_file = NULL;
    size_t num_keys = 500000000;
    int query_pct = 100;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc)
            workload_file = argv[++i];
        else if (!strcmp(argv[i], "-N") && i + 1 < argc)
            num_keys = (size_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc)
            host = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc)
            auth = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            name = argv[++i];
        else if (!strcmp(argv[i], "-Q") && i + 1 < argc) {
            query_pct = atoi(argv[++i]);
            if (query_pct < 1) query_pct = 1;
            if (query_pct > 100) query_pct = 100;
        } else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-help") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (!workload_file) {
        fprintf(stderr, "Error: workload file required (-f)\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Connect */
    redisContext *ctx = redisConnect(host, port);
    if (!ctx || ctx->err) {
        fprintf(stderr, "Error: cannot connect to Redis at %s:%d: %s\n",
                host, port, ctx ? ctx->errstr : "OOM");
        if (ctx) redisFree(ctx);
        return 1;
    }

    if (auth) {
        redisReply *r = redisCommand(ctx, "AUTH %s", auth);
        if (!r || r->type == REDIS_REPLY_ERROR) {
            fprintf(stderr, "AUTH failed: %s\n",
                    r ? r->str : "no reply");
            if (r) freeReplyObject(r);
            redisFree(ctx);
            return 1;
        }
        freeReplyObject(r);
    }

    if (verbose) {
        printf("============================================================\n");
        printf("Airport Operations Redis Benchmark\n");
        printf("============================================================\n");
        printf("Host:          %s:%d\n", host, port);
        printf("Workload:      %s\n", workload_file);
        printf("Flights (N):   %zu\n", num_keys);
        printf("Query gates:   %d%%\n", query_pct);
        printf("Tree label:    %s\n", name);
        printf("============================================================\n\n");
        printf("INSERT PHASE: Dispatch center loading full day's flight schedule...\n");
    }

    /* Load workload */
    uint32_t *keys = read_workload(workload_file, num_keys);
    if (!keys) { redisFree(ctx); return 1; }

    /* ── INSERT: load flight schedule into gate streams ─────────────────── */
    size_t num_gates = 0;
    long long insert_ns = run_insert(ctx, keys, num_keys, verbose, &num_gates);

    if (verbose)
        printf("  Loaded %zu flight events into %zu active gates  [%lld ns, %.2f ns/flight]\n\n",
               num_keys, num_gates, insert_ns, (double)insert_ns / num_keys);

    /* ── QUERY: gate agent / departure board refresh ──────────────────────── */
    if (verbose)
        printf("QUERY PHASE: Gate agents and departure boards refreshing schedules...\n");

    uint64_t qcount = (uint64_t)num_gates * (uint64_t)query_pct / 100;
    if (qcount == 0) qcount = 1;
    size_t query_count = (size_t)qcount;

    long long query_ns = run_query(ctx, num_gates, query_count, verbose);

    if (verbose)
        printf("  Queried %zu of %zu gates  [%lld ns, %.2f ns/gate]\n\n",
               query_count, num_gates, query_ns,
               (double)query_ns / query_count);

    /* ── CLEANUP: end-of-day gate stream purge ───────────────────────────── */
    if (verbose)
        printf("CLEANUP: Purging %zu gate streams (end-of-day reset)...\n", num_gates);
    cleanup_gates(ctx, num_gates, verbose);

    /* ── results ──────────────────────────────────────────────────────────── */
    if (verbose) {
        printf("\n============================================================\n");
        printf("Results — %s\n", name);
        printf("============================================================\n");
        printf("  Flights loaded : %zu  (across %zu gates)\n", num_keys, num_gates);
        printf("  Insert time    : %lld ns  (%.2f ns/flight)\n",
               insert_ns, (double)insert_ns / num_keys);
        printf("  Query time     : %lld ns  (%.2f ns/gate, %zu gates queried)\n",
               query_ns, (double)query_ns / query_count, query_count);
        printf("\nCSV Output:\n");
    }

    printf("tree_type,insert_ns,query_ns\n");
    printf("%s,%lld,%lld\n", name, insert_ns, query_ns);

    zfree(keys);
    redisFree(ctx);
    return 0;
}
