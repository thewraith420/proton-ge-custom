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

static void read_stats(long *sgrant, long *syield) {
    FILE *f = fopen("/sys/kernel/debug/rseq/stats", "r");
    if (!f) { *sgrant = *syield = -1; return; }

    char line[256];
    *sgrant = *syield = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "sgrant:", 7) == 0) sscanf(line + 7, "%ld", sgrant);
        else if (strncmp(line, "syield:", 7) == 0) sscanf(line + 7, "%ld", syield);
    }
    fclose(f);
}

/* CPU stress child - just burns CPU to create pressure */
void stress_child(void) {
    volatile unsigned long x = 0;
    while (1) {
        for (int i = 0; i < 1000000; i++) x++;
    }
}

int main(void) {
    struct rseq *r = get_rseq();
    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    printf("=== Yield Under CPU Pressure Test ===\n");
    printf("CPUs: %d\n\n", num_cpus);

    // Enable extension
    prctl(PR_RSEQ_SLICE_EXTENSION, PR_RSEQ_SLICE_EXTENSION_SET, PR_RSEQ_SLICE_EXT_ENABLE, 0, 0);

    // Fork stress children to create CPU pressure
    printf("Spawning %d CPU stress children...\n", num_cpus * 2);
    pid_t children[64];
    for (int i = 0; i < num_cpus * 2 && i < 64; i++) {
        children[i] = fork();
        if (children[i] == 0) {
            stress_child();
            exit(0);
        }
    }

    sleep(1); // Let stress build up

    long grant_before, yield_before;
    read_stats(&grant_before, &yield_before);
    printf("Stats before: sgrant=%ld syield=%ld\n\n", grant_before, yield_before);

    int yields_returned_1 = 0;
    int total_attempts = 0;

    printf("Running main test loop under pressure...\n");

    time_t start = time(NULL);
    while (time(NULL) - start < 5) {
        // Set request
        r->slice_ctrl.request = 1;
        __asm__ __volatile__("" ::: "memory");

        // Do some work - this is when we might get preempted and granted
        volatile unsigned long work = 0;
        for (int j = 0; j < 50000; j++) work++;

        // Check if granted WHILE doing work (tight poll)
        for (int check = 0; check < 100; check++) {
            __asm__ __volatile__("" ::: "memory");
            if (r->slice_ctrl.granted) {
                // Got grant! Immediately yield
                long ret = syscall(__NR_rseq_slice_yield);
                total_attempts++;
                if (ret == 1) {
                    yields_returned_1++;
                    printf("  GOT YIELD ret=1!\n");
                }
                r->slice_ctrl.granted = 0;
                break;
            }
            // Tiny bit more work between checks
            for (int w = 0; w < 100; w++) work++;
        }

        r->slice_ctrl.request = 0;
    }

    printf("\nTest done. Killing stress children...\n");
    for (int i = 0; i < num_cpus * 2 && i < 64; i++) {
        kill(children[i], 9);
        waitpid(children[i], NULL, 0);
    }

    long grant_after, yield_after;
    read_stats(&grant_after, &yield_after);

    printf("\n=== Results ===\n");
    printf("Yield syscall attempts: %d\n", total_attempts);
    printf("Yields returned 1: %d\n", yields_returned_1);
    printf("Stats delta: sgrant=+%ld syield=+%ld\n",
           grant_after - grant_before, yield_after - yield_before);

    return 0;
}
