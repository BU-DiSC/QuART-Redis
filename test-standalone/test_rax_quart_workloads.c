/* Test rax_quart with different workload files
 * 
 * Usage: ./test-rax-quart-workloads -f <workload_file> [-N <num_keys>] [-v]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../src/rax.h"
#include "../src/rax_quart.h"
#include "../src/zmalloc.h"

#define DEFAULT_QUERY_PERCENTAGE 100  // Query 100% of keys by default

/* Read binary workload file */
uint32_t* read_workload(const char *filename, size_t num_keys) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open file %s\n", filename);
        return NULL;
    }
    
    uint32_t *keys = zmalloc(num_keys * sizeof(uint32_t));
    if (!keys) {
        fprintf(stderr, "Error: Cannot allocate memory\n");
        fclose(fp);
        return NULL;
    }
    
    size_t read = fread(keys, sizeof(uint32_t), num_keys, fp);
    fclose(fp);
    
    if (read != num_keys) {
        fprintf(stderr, "Warning: Read %zu keys instead of %zu\n", read, num_keys);
    }
    
    return keys;
}

/* Get current time in nanoseconds */
long long get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

/* Test standard rax */
void test_rax(uint32_t *keys, size_t num_keys, size_t query_count, int verbose,
              long long *insert_time, long long *query_time) {
    rax *r = raxNew();
    unsigned char key_bytes[4];
    
    // Insertion test
    long long start = get_time_ns();
    for (size_t i = 0; i < num_keys; i++) {
        key_bytes[0] = (keys[i] >> 24) & 0xFF;
        key_bytes[1] = (keys[i] >> 16) & 0xFF;
        key_bytes[2] = (keys[i] >> 8) & 0xFF;
        key_bytes[3] = keys[i] & 0xFF;
        
        raxInsert(r, key_bytes, 4, (void*)(uintptr_t)(i+1), NULL);
    }
    long long end = get_time_ns();
    *insert_time = end - start;
    
    if (verbose) {
        printf("  RAX: Inserted %zu keys in %lld ns (%.2f ns/key)\n", 
               num_keys, *insert_time, (double)*insert_time / num_keys);
    }
    
    // Query test
    if (query_count == 0) query_count = 1;
    if (query_count > num_keys) query_count = num_keys;
    size_t stride = num_keys / query_count;
    if (stride == 0) stride = 1;
    
    start = get_time_ns();
    for (size_t i = 0; i < query_count; i++) {
        size_t idx = i * stride;
        if (idx >= num_keys) idx = num_keys - 1;
        key_bytes[0] = (keys[idx] >> 24) & 0xFF;
        key_bytes[1] = (keys[idx] >> 16) & 0xFF;
        key_bytes[2] = (keys[idx] >> 8) & 0xFF;
        key_bytes[3] = keys[idx] & 0xFF;
        
        void *value;
        raxFind(r, key_bytes, 4, &value);
    }
    end = get_time_ns();
    *query_time = end - start;
    
    if (verbose) {
        printf("  RAX: Queried %zu keys in %lld ns (%.2f ns/query)\n",
               query_count, *query_time, (double)*query_time / query_count);
    }
    
    raxFree(r);
}

/* Test rax_quart */
void test_rax_quart(uint32_t *keys, size_t num_keys, size_t query_count, int verbose,
                    long long *insert_time, long long *query_time) {
    raxQuart *rq = raxQuartNew();
    unsigned char key_bytes[4];
    
    // Insertion test
    long long start = get_time_ns();
    for (size_t i = 0; i < num_keys; i++) {
        key_bytes[0] = (keys[i] >> 24) & 0xFF;
        key_bytes[1] = (keys[i] >> 16) & 0xFF;
        key_bytes[2] = (keys[i] >> 8) & 0xFF;
        key_bytes[3] = keys[i] & 0xFF;
        raxQuartInsert(rq, key_bytes, 4, (void*)(uintptr_t)(i+1), NULL);
    }
    long long end = get_time_ns();
    *insert_time = end - start;
    
    if (verbose) {
        printf("  QuART: Inserted %zu keys in %lld ns (%.2f ns/key)\n",
               num_keys, *insert_time, (double)*insert_time / num_keys);
    }
    
    // Query test
    if (query_count == 0) query_count = 1;
    if (query_count > num_keys) query_count = num_keys;
    size_t stride = num_keys / query_count;
    if (stride == 0) stride = 1;
    
    start = get_time_ns();
    for (size_t i = 0; i < query_count; i++) {
        size_t idx = i * stride;
        if (idx >= num_keys) idx = num_keys - 1;
        key_bytes[0] = (keys[idx] >> 24) & 0xFF;
        key_bytes[1] = (keys[idx] >> 16) & 0xFF;
        key_bytes[2] = (keys[idx] >> 8) & 0xFF;
        key_bytes[3] = keys[idx] & 0xFF;
        void *value;
        raxQuartFind(rq, key_bytes, 4, &value);
    }
    end = get_time_ns();
    *query_time = end - start;
    
    if (verbose) {
        printf("  QuART: Queried %zu keys in %lld ns (%.2f ns/query)\n",
               query_count, *query_time, (double)*query_time / query_count);
    }
    
    raxQuartFree(rq);
}

void print_usage(const char *progname) {
    printf("Usage: %s -f <workload_file> [-N <num_keys>] [-v]\n", progname);
    printf("Options:\n");
    printf("  -f <file>     Path to binary workload file (required)\n");
    printf("  -N <number>   Number of keys to read (default: 500000000)\n");
    printf("  -Q <pct>      Query percentage (1-100, default: %d; 100 = query all keys)\n", DEFAULT_QUERY_PERCENTAGE);
    printf("  -v            Verbose output\n");
    printf("\nOutput Format (CSV):\n");
    printf("tree_type,insert_ns,query_ns\n");
}

int main(int argc, char **argv) {
    char *workload_file = NULL;
    size_t num_keys = 500000000;
    int verbose = 0;
    int query_pct = DEFAULT_QUERY_PERCENTAGE;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            workload_file = argv[++i];
        } else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc) {
            num_keys = atoll(argv[++i]);
        } else if (strcmp(argv[i], "-Q") == 0 && i + 1 < argc) {
            query_pct = atoi(argv[++i]);
            if (query_pct < 1) query_pct = 1;
            if (query_pct > 100) query_pct = 100;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    if (!workload_file) {
        fprintf(stderr, "Error: Workload file required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    if (verbose) {
        printf("========================================\n");
        printf("RAX vs RAX_QUART Workload Test\n");
        printf("========================================\n");
        printf("Workload file: %s\n", workload_file);
        printf("Number of keys: %zu\n", num_keys);
        printf("Query percent: %d%%\n", query_pct);
        printf("========================================\n\n");
    }

    size_t query_count;
    uint64_t nk = (uint64_t)num_keys;
    uint64_t qp = (uint64_t)query_pct;
    if (qp != 0 && nk > UINT64_MAX / qp) {
        query_count = num_keys;
    } else {
        query_count = (size_t)(nk * qp / 100);
    }
    if (query_count == 0) query_count = 1;
    
    // Read workload once in the parent; children inherit it via fork() copy-on-write.
    uint32_t *keys = read_workload(workload_file, num_keys);
    if (!keys) {
        return 1;
    }

    long long rax_insert_time = 0, rax_query_time = 0;
    long long quart_insert_time = 0, quart_query_time = 0;

    /* Each tree runs in its own child process so that:
     *  - jemalloc starts from an identical baseline for both trees
     *    (only the read-only keys[] array has been allocated)
     *  - the insertion phase of one tree cannot pollute the CPU caches
     *    seen during the query phase of the other tree
     * The two children run sequentially (fork → wait) to avoid CPU contention.
     * Results are returned to the parent through a pipe.
     */

    /* ── RAX child ──────────────────────────────────────────────────── */
    int pfd[2];
    if (pipe(pfd) != 0) { perror("pipe"); return 1; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        close(pfd[0]);
        long long ins, qry;
        if (verbose) printf("Testing standard RAX...\n");
        test_rax(keys, num_keys, query_count, verbose, &ins, &qry);
        write(pfd[1], &ins, sizeof(ins));
        write(pfd[1], &qry, sizeof(qry));
        close(pfd[1]);
        zfree(keys);
        exit(0);
    }
    close(pfd[1]);
    if (read(pfd[0], &rax_insert_time, sizeof(rax_insert_time)) != (ssize_t)sizeof(rax_insert_time) ||
        read(pfd[0], &rax_query_time,  sizeof(rax_query_time))  != (ssize_t)sizeof(rax_query_time)) {
        fprintf(stderr, "Error: failed to read RAX results from child\n");
        return 1;
    }
    close(pfd[0]);
    waitpid(pid, NULL, 0);

    /* ── QuART child ─────────────────────────────────────────────────── */
    if (pipe(pfd) != 0) { perror("pipe"); return 1; }
    pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        close(pfd[0]);
        long long ins, qry;
        if (verbose) printf("\nTesting RAX_QUART...\n");
        test_rax_quart(keys, num_keys, query_count, verbose, &ins, &qry);
        write(pfd[1], &ins, sizeof(ins));
        write(pfd[1], &qry, sizeof(qry));
        close(pfd[1]);
        zfree(keys);
        exit(0);
    }
    close(pfd[1]);
    if (read(pfd[0], &quart_insert_time, sizeof(quart_insert_time)) != (ssize_t)sizeof(quart_insert_time) ||
        read(pfd[0], &quart_query_time,  sizeof(quart_query_time))  != (ssize_t)sizeof(quart_query_time)) {
        fprintf(stderr, "Error: failed to read QuART results from child\n");
        return 1;
    }
    close(pfd[0]);
    waitpid(pid, NULL, 0);

    // Print results
    if (verbose) {
        printf("\n========================================\n");
        printf("Results Summary\n");
        printf("========================================\n");
        printf("RAX:\n");
        printf("  Insert: %lld ns (%.2f ns/key)\n", rax_insert_time, (double)rax_insert_time/num_keys);
        printf("  Query:  %lld ns\n", rax_query_time);
        printf("\nQuART:\n");
        printf("  Insert: %lld ns (%.2f ns/key)\n", quart_insert_time, (double)quart_insert_time/num_keys);
        printf("  Query:  %lld ns\n", quart_query_time);
        printf("  Speedup: %.2fx (insert), %.2fx (query)\n",
               (double)rax_insert_time/quart_insert_time,
               (double)rax_query_time/quart_query_time);
        printf("\n========================================\n");
        printf("CSV Output:\n");
    }
    
    // CSV output
    printf("tree_type,insert_ns,query_ns\n");
    printf("RAX,%lld,%lld\n", rax_insert_time, rax_query_time);
    printf("QuART,%lld,%lld\n", quart_insert_time, quart_query_time);
    
    zfree(keys);
    return 0;
}
