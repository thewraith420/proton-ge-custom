#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sched.h>
#include <time.h>
#include <string.h>

#define PR_RSEQ_SLICE_EXTENSION 79
#define PR_RSEQ_SLICE_EXTENSION_SET 2
#define PR_RSEQ_SLICE_EXT_ENABLE 0x01
#define __NR_rseq_slice_yield 471

struct rseq_slice_ctrl {
    union {
        uint32_t all;
        struct {
            uint8_t request;
            uint8_t granted;
            uint16_t __reserved;
        };
    };
};

struct rseq {
    uint32_t cpu_id_start;
    uint32_t cpu_id;
    uint64_t rseq_cs;
    uint32_t flags;
    uint32_t node_id;
    uint32_t mm_cid;
    struct rseq_slice_ctrl slice_ctrl;
} __attribute__((aligned(32)));

extern ptrdiff_t __rseq_offset __attribute__((weak));

static struct rseq *get_rseq(void) {
    char *tls_base;
    asm("mov %%fs:0, %0" : "=r"(tls_base));
    return (struct rseq *)(tls_base + __rseq_offset);
}

/* Read kernel stats */
static void read_stats(long *sgrant, long *syield, long *sexpir) {
    FILE *f = fopen("/sys/kernel/debug/rseq/stats", "r");
    if (!f) {
        *sgrant = *syield = *sexpir = -1;
        return;
    }

    char line[256];
    *sgrant = *syield = *sexpir = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "sgrant:", 7) == 0) sscanf(line + 7, "%ld", sgrant);
        else if (strncmp(line, "syield:", 7) == 0) sscanf(line + 7, "%ld", syield);
        else if (strncmp(line, "sexpir:", 7) == 0) sscanf(line + 7, "%ld", sexpir);
    }
    fclose(f);
}

int main(void) {
    struct rseq *r = get_rseq();

    printf("=== Early Yield Test ===\n");
    printf("Goal: Call yield syscall BEFORE being scheduled out\n\n");

    // Enable extension
    int rc = prctl(PR_RSEQ_SLICE_EXTENSION, PR_RSEQ_SLICE_EXTENSION_SET, PR_RSEQ_SLICE_EXT_ENABLE, 0, 0);
    if (rc < 0) {
        printf("Failed to enable slice extension\n");
        return 1;
    }
    printf("Slice extension enabled\n");

    long grant_before, yield_before, expir_before;
    long grant_after, yield_after, expir_after;

    read_stats(&grant_before, &yield_before, &expir_before);
    printf("Stats before: sgrant=%ld syield=%ld sexpir=%ld\n\n",
           grant_before, yield_before, expir_before);

    int yields_returned_1 = 0;
    int total_yield_calls = 0;
    int grants_observed = 0;

    printf("Strategy: Set request, yield CPU to get grant, then IMMEDIATELY\n");
    printf("call yield syscall before doing anything else.\n\n");

    for (int i = 0; i < 10000; i++) {
        // Set request BEFORE yielding
        r->slice_ctrl.request = 1;
        __asm__ __volatile__("" ::: "memory");

        // Yield CPU - this should trigger a grant when we come back
        sched_yield();

        // IMMEDIATELY call the yield syscall - don't check anything first
        // The idea: maybe we can catch the window before sched_switch is processed
        long ret = syscall(__NR_rseq_slice_yield);
        total_yield_calls++;

        if (ret == 1) {
            yields_returned_1++;
        }

        // Now check if we saw a grant
        __asm__ __volatile__("" ::: "memory");
        if (r->slice_ctrl.granted) {
            grants_observed++;
            r->slice_ctrl.granted = 0;
        }

        r->slice_ctrl.request = 0;
    }

    printf("Test 1 Results (yield immediately after sched_yield):\n");
    printf("  Yield calls: %d\n", total_yield_calls);
    printf("  Yields returned 1: %d\n", yields_returned_1);
    printf("  Grants observed: %d\n\n", grants_observed);

    // Test 2: Try calling yield BEFORE sched_yield (proactive)
    printf("Test 2: Proactive yield (call yield before being scheduled out)\n");

    yields_returned_1 = 0;
    total_yield_calls = 0;
    grants_observed = 0;

    for (int i = 0; i < 10000; i++) {
        // Set request
        r->slice_ctrl.request = 1;
        __asm__ __volatile__("" ::: "memory");

        // Do minimal work
        volatile int x = 0;
        for (int j = 0; j < 100; j++) x++;

        // Call yield proactively BEFORE any scheduling
        long ret = syscall(__NR_rseq_slice_yield);
        total_yield_calls++;

        if (ret == 1) {
            yields_returned_1++;
        }

        // Check grant after
        __asm__ __volatile__("" ::: "memory");
        if (r->slice_ctrl.granted) {
            grants_observed++;
            r->slice_ctrl.granted = 0;
        }

        r->slice_ctrl.request = 0;

        // Small sleep to allow other processes to run
        usleep(100);
    }

    printf("  Yield calls: %d\n", total_yield_calls);
    printf("  Yields returned 1: %d\n", yields_returned_1);
    printf("  Grants observed: %d\n\n", grants_observed);

    // Test 3: Busy loop waiting for grant, then yield immediately
    printf("Test 3: Busy-wait for grant in tight loop, yield when seen\n");

    yields_returned_1 = 0;
    total_yield_calls = 0;
    grants_observed = 0;
    int timeouts = 0;

    for (int i = 0; i < 1000; i++) {
        r->slice_ctrl.request = 1;
        __asm__ __volatile__("" ::: "memory");

        // Busy wait for grant (with timeout)
        int got_grant = 0;
        for (int wait = 0; wait < 100000; wait++) {
            __asm__ __volatile__("" ::: "memory");
            if (r->slice_ctrl.granted) {
                got_grant = 1;
                grants_observed++;
                // IMMEDIATELY yield
                long ret = syscall(__NR_rseq_slice_yield);
                total_yield_calls++;
                if (ret == 1) yields_returned_1++;
                r->slice_ctrl.granted = 0;
                break;
            }
        }

        if (!got_grant) timeouts++;

        r->slice_ctrl.request = 0;
        sched_yield(); // Let other things run
    }

    printf("  Iterations: 1000\n");
    printf("  Grants observed: %d\n", grants_observed);
    printf("  Timeouts: %d\n", timeouts);
    printf("  Yield calls: %d\n", total_yield_calls);
    printf("  Yields returned 1: %d\n\n", yields_returned_1);

    read_stats(&grant_after, &yield_after, &expir_after);
    printf("Stats after: sgrant=%ld syield=%ld sexpir=%ld\n",
           grant_after, yield_after, expir_after);
    printf("Delta: sgrant=+%ld syield=+%ld sexpir=+%ld\n",
           grant_after - grant_before, yield_after - yield_before, expir_after - expir_before);

    return 0;
}
