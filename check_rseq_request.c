#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

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

struct rseq_abi {
    uint32_t cpu_id_start;
    uint32_t cpu_id;
    uint64_t rseq_cs;
    uint32_t flags;
    uint32_t node_id;
    uint32_t mm_cid;
    struct rseq_slice_ctrl slice_ctrl;
} __attribute__((aligned(32)));

extern ptrdiff_t __rseq_offset __attribute__((weak));

static struct rseq_abi *get_rseq(void) {
    if (&__rseq_offset == NULL || __rseq_offset == 0)
        return NULL;
    char *tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
    return (struct rseq_abi *)(tp + __rseq_offset);
}

int main(void) {
    struct rseq_abi *r = get_rseq();

    if (!r) {
        printf("RSEQ not available\n");
        return 1;
    }

    printf("RSEQ Status Check\n");
    printf("==================\n");
    printf("CPU ID: %u\n", r->cpu_id);
    printf("Flags: 0x%x\n", r->flags);
    printf("slice_ctrl.all: 0x%08x\n", r->slice_ctrl.all);
    printf("slice_ctrl.request: %u\n", r->slice_ctrl.request);
    printf("slice_ctrl.granted: %u\n", r->slice_ctrl.granted);

    if (r->slice_ctrl.request == 1) {
        printf("\n✓ Request bit IS SET - thread will receive grants\n");
    } else {
        printf("\n✗ Request bit NOT SET - thread won't receive grants\n");
    }

    return 0;
}
