#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <time.h>

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

int main(void) {
    struct rseq *r = get_rseq();

    // Enable extension
    int rc = prctl(PR_RSEQ_SLICE_EXTENSION, PR_RSEQ_SLICE_EXTENSION_SET, PR_RSEQ_SLICE_EXT_ENABLE, 0, 0);
    if (rc < 0) {
        printf("Failed to enable slice extension\n");
        return 1;
    }
    printf("Slice extension enabled\n\n");

    int total_grants = 0;
    int total_yields = 0;  // syscall returned 1
    int total_calls = 0;

    printf("Testing for 3 seconds - tracking syscall return values...\n\n");

    time_t start = time(NULL);
    while (time(NULL) - start < 3) {
        // Set request
        r->slice_ctrl.request = 1;
        __asm__ __volatile__("" ::: "memory");

        // Busy work
        volatile unsigned long x = 0;
        for (int j = 0; j < 10000; j++) x++;

        // Call yield and check return
        long ret = syscall(__NR_rseq_slice_yield);
        total_calls++;

        if (ret == 1) {
            total_yields++;
            printf("  YIELD ret=1 at call #%d\n", total_calls);
        }

        // Check granted
        __asm__ __volatile__("" ::: "memory");
        if (r->slice_ctrl.granted) {
            total_grants++;
            r->slice_ctrl.granted = 0;
        }

        r->slice_ctrl.request = 0;
    }

    printf("\n=== RESULTS ===\n");
    printf("Total syscall calls: %d\n", total_calls);
    printf("Total grants seen:   %d\n", total_grants);
    printf("Yields (ret=1):      %d\n", total_yields);
    printf("\nIf yields=0 but grants>0, we're missing the window.\n");
    printf("The syscall return value IS the program-level yield detection.\n");

    return 0;
}
