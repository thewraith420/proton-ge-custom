#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sched.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

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

    printf("=== Direct Yield Return Value Test ===\n\n");

    // Enable extension
    int rc = prctl(PR_RSEQ_SLICE_EXTENSION, PR_RSEQ_SLICE_EXTENSION_SET, PR_RSEQ_SLICE_EXT_ENABLE, 0, 0);
    if (rc < 0) {
        printf("Failed to enable slice extension: %s\n", strerror(errno));
        return 1;
    }
    printf("Slice extension enabled\n");
    printf("Flags: 0x%x (AVAILABLE=0x10, ENABLED=0x20)\n\n", r->flags);

    printf("Test 1: Yield with NO grant (should return 0)\n");
    {
        r->slice_ctrl.all = 0;  // No request, no grant
        long ret = syscall(__NR_rseq_slice_yield);
        printf("  Result: ret=%ld (expected 0)\n", ret);
        if (ret != 0) printf("  *** UNEXPECTED ***\n");
    }

    printf("\nTest 2: Yield with request but no grant (should return 0)\n");
    {
        r->slice_ctrl.all = 0;
        r->slice_ctrl.request = 1;
        __asm__ __volatile__("" ::: "memory");
        long ret = syscall(__NR_rseq_slice_yield);
        printf("  Result: ret=%ld (expected 0)\n", ret);
        if (ret != 0) printf("  *** UNEXPECTED ***\n");
    }

    printf("\nTest 3: Force granted=1 in userspace, then yield\n");
    printf("  (Kernel checks internal state, not userspace value)\n");
    {
        r->slice_ctrl.all = 0;
        r->slice_ctrl.request = 1;
        r->slice_ctrl.granted = 1;  // Force it
        __asm__ __volatile__("" ::: "memory");
        long ret = syscall(__NR_rseq_slice_yield);
        printf("  Result: ret=%ld (expected 0 - kernel ignores userspace granted)\n", ret);
    }

    printf("\nTest 4: Syscall error check\n");
    {
        // Wrong syscall number to verify errors work
        long ret = syscall(99999);
        printf("  Invalid syscall: ret=%ld, errno=%d (%s)\n", ret, errno, strerror(errno));
    }

    printf("\n=== Analysis ===\n");
    printf("The yield syscall returns 1 ONLY if:\n");
    printf("  1. There's an active grant in kernel state (not userspace)\n");
    printf("  2. sched_switch flag is false (task wasn't scheduled out)\n");
    printf("  3. The syscall is called while still running continuously\n\n");

    printf("The problem: By the time userspace can detect a grant,\n");
    printf("the task has already been through a schedule event\n");
    printf("(sched_switch=true), so yields always fail.\n\n");

    printf("This might be a design issue in the kernel implementation\n");
    printf("where the yield path is effectively unreachable.\n");

    return 0;
}
