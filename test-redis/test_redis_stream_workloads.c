/* Redis Stream Workload Benchmark
 *
 * Benchmarks Redis stream INSERT (XADD) and QUERY (XRANGE) using the
 * binary workload files produced by bods.  Each uint32 value is used
 * as both the stream entry ID and the single field value.
 *
 * The workload is split across monotonically-increasing sub-streams
 * (stream:0, stream:1, …).  Whenever the next key is smaller than the
 * previous one a new sub-stream is started — matching the rax "bridge"
 * behaviour that QuART's fast-path targets.
 *
 *   K=0   fully sorted   → one stream, every XADD hits QuART fast-path
 *   K=100 fully random   → many streams, no fast-path benefit
 *
 * Usage:
 *   ./test-redis-stream-workloads -f <workload_file> -N <num_keys>
 *                                 [-h <host>] [-p <port>] [-a <auth>]
 *                                 [-n <name>] [-Q <query_pct>]
 *                                 [-C <xrange_count>] [-v]
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

/* Number of XRANGE commands to pipeline before draining replies. */
#define PIPELINE_DEPTH 1024

/* ── helpers ─────────────────────────────────────────────────────────────── */

static long long get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

/* Delete all sub-streams created during the test. */
static void cleanup_streams(redisContext *ctx, size_t num_streams, int verbose) {
    size_t errors = 0;

    for (size_t i = 0; i < num_streams; i++) {
        char key[32];
        snprintf(key, sizeof(key), "%zu", i);
        redisReply *r = redisCommand(ctx, "DEL stream:%s", key);
        if (r) freeReplyObject(r);
        else errors++;
    }
    if (verbose && errors)
        fprintf(stderr, "  Warning: %zu DEL errors during cleanup\n", errors);
}

/* ── INSERT ───────────────────────────────────────────────────────────────── */
/*
 * Issues a single DEBUG STREAM-BULK-INSERT command.  The server reads the
 * workload file itself, calls streamAppendItem for all N keys in one tight
 * loop, and returns [num_streams, insert_ns, errors].  There is zero
 * client-server overhead inside the measured window, so the timing reflects
 * pure rax/QuART index insertion — making the RAX vs QuART difference
 * directly visible.
 */
static long long run_insert(redisContext *ctx, const char *filename,
                            size_t num_keys, int verbose, size_t *out_streams) {
    if (verbose)
        printf("  Inserting %zu keys via DEBUG STREAM-BULK-INSERT...\n", num_keys);

    redisReply *r = redisCommand(ctx, "DEBUG STREAM-BULK-INSERT %s %llu",
                                 filename, (unsigned long long)num_keys);
    if (!r) {
        fprintf(stderr, "Error: no reply from DEBUG STREAM-BULK-INSERT\n");
        return -1;
    }
    if (r->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "Error from server: %s\n", r->str);
        freeReplyObject(r);
        return -1;
    }
    if (r->type != REDIS_REPLY_ARRAY || r->elements < 3) {
        fprintf(stderr, "Error: unexpected reply type %d\n", r->type);
        freeReplyObject(r);
        return -1;
    }

    *out_streams = (size_t)r->element[0]->integer;
    long long insert_ns = r->element[1]->integer;
    long long errors    = r->element[2]->integer;
    freeReplyObject(r);

    if (errors && verbose)
        fprintf(stderr, "  Warning: %lld streamAppendItem errors\n", errors);

    return insert_ns;
}

/* ── QUERY ────────────────────────────────────────────────────────────────── */
/*
 * Issues XRANGE stream:<idx> - + COUNT <n> for a sampled subset of
 * sub-streams, pipelined in batches of PIPELINE_DEPTH.
 */
static long long run_query(redisContext *ctx, size_t num_streams,
                           size_t query_count, size_t xrange_count, int verbose) {
    if (query_count > num_streams) query_count = num_streams;
    if (query_count == 0) query_count = 1;

    size_t stride = num_streams / query_count;
    if (stride == 0) stride = 1;

    size_t errors = 0;
    size_t pending = 0;

    char cntstr[24];
    snprintf(cntstr, sizeof(cntstr), "%zu", xrange_count);

    if (verbose)
        printf("  Querying %zu streams (XRANGE COUNT %zu, pipeline depth %d)...\n",
               query_count, xrange_count, PIPELINE_DEPTH);

    long long start = get_time_ns();

    for (size_t i = 0; i < query_count; i++) {
        size_t idx = i * stride;
        if (idx >= num_streams) idx = num_streams - 1;
        char skey[24];
        snprintf(skey, sizeof(skey), "%zu", idx);

        redisAppendCommand(ctx, "XRANGE stream:%s - + COUNT %s", skey, cntstr);
        pending++;

        if (pending >= PIPELINE_DEPTH) {
            for (size_t j = 0; j < pending; j++) {
                redisReply *r = NULL;
                redisGetReply(ctx, (void **)&r);
                if (!r || r->type == REDIS_REPLY_ERROR) errors++;
                if (r) freeReplyObject(r);
            }
            pending = 0;
        }
    }

    /* Drain remaining */
    for (size_t j = 0; j < pending; j++) {
        redisReply *r = NULL;
        redisGetReply(ctx, (void **)&r);
        if (!r || r->type == REDIS_REPLY_ERROR) errors++;
        if (r) freeReplyObject(r);
    }

    long long end = get_time_ns();

    if (errors && verbose)
        fprintf(stderr, "  Warning: %zu XRANGE errors\n", errors);

    return end - start;
}

/* ── usage ───────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    printf("Redis Stream Workload Benchmark\n");
    printf("================================\n");
    printf("Benchmarks XADD and XRANGE against a running Redis using binary\n");
    printf("workload files (uint32 arrays).  Each value is used as both the\n");
    printf("stream entry ID and the single field value.  Sub-streams are\n");
    printf("created at each descending-key boundary (rax bridge).\n\n");
    printf("Usage: %s -f <workload_file> -N <num_keys>\n", prog);
    printf("            [-h <host>] [-p <port>] [-a <auth>]\n");
    printf("            [-n <name>] [-Q <query_pct>] [-C <xrange_count>] [-v]\n\n");
    printf("Options:\n");
    printf("  -f <file>    Binary workload file (uint32_t array, required)\n");
    printf("  -N <n>       Number of keys to insert (default: 50000000)\n");
    printf("  -h <host>    Redis host (default: 127.0.0.1)\n");
    printf("  -p <port>    Redis port (default: 6379)\n");
    printf("  -a <pass>    Redis AUTH password (optional)\n");
    printf("  -n <name>    Label for CSV tree_type column (default: REDIS)\n");
    printf("  -Q <pct>     Percentage of sub-streams to query (1-100, default: 100)\n");
    printf("  -C <count>   Max entries per XRANGE reply (default: 1000)\n");
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
    size_t num_keys = 50000000;
    int query_pct = 100;
    size_t xrange_count = 1000;
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
        } else if (!strcmp(argv[i], "-C") && i + 1 < argc) {
            xrange_count = (size_t)atoll(argv[++i]);
            if (xrange_count < 1) xrange_count = 1;
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
        printf("Redis Stream Workload Benchmark\n");
        printf("============================================================\n");
        printf("Host:         %s:%d\n", host, port);
        printf("Workload:     %s\n", workload_file);
        printf("Keys (N):     %zu\n", num_keys);
        printf("Query pct:    %d%%\n", query_pct);
        printf("XRANGE COUNT: %zu\n", xrange_count);
        printf("Tree label:   %s\n", name);
        printf("============================================================\n\n");
        printf("INSERT phase...\n");
    }

    /* ── INSERT ──────────────────────────────────────────────────────────── */
    size_t num_streams = 0;
    long long insert_ns = run_insert(ctx, workload_file, num_keys, verbose, &num_streams);
    if (insert_ns < 0) { redisFree(ctx); return 1; }

    if (verbose)
        printf("  Inserted %zu keys into %zu sub-streams  [%lld ns, %.2f ns/key]\n\n",
               num_keys, num_streams, insert_ns, (double)insert_ns / num_keys);

    /* ── QUERY ───────────────────────────────────────────────────────────── */
    if (verbose)
        printf("QUERY phase...\n");

    uint64_t qcount = (uint64_t)num_streams * (uint64_t)query_pct / 100;
    if (qcount == 0) qcount = 1;
    size_t query_count = (size_t)qcount;

    long long query_ns = run_query(ctx, num_streams, query_count, xrange_count, verbose);

    if (verbose)
        printf("  Queried %zu of %zu streams  [%lld ns, %.2f ns/stream]\n\n",
               query_count, num_streams, query_ns,
               (double)query_ns / query_count);

    /* ── CLEANUP ─────────────────────────────────────────────────────────── */
    if (verbose)
        printf("CLEANUP: deleting %zu streams...\n", num_streams);
    cleanup_streams(ctx, num_streams, verbose);

    /* ── results ─────────────────────────────────────────────────────────── */
    if (verbose) {
        printf("\n============================================================\n");
        printf("Results — %s\n", name);
        printf("============================================================\n");
        printf("  Keys inserted  : %zu  (across %zu sub-streams)\n", num_keys, num_streams);
        printf("  Insert time    : %lld ns  (%.2f ns/key)\n",
               insert_ns, (double)insert_ns / num_keys);
        printf("  Query time     : %lld ns  (%.2f ns/stream, %zu streams queried)\n",
               query_ns, (double)query_ns / query_count, query_count);
        printf("\nCSV Output:\n");
    }

    printf("tree_type,insert_ns,query_ns\n");
    printf("%s,%lld,%lld\n", name, insert_ns, query_ns);

    redisFree(ctx);
    return 0;
}
