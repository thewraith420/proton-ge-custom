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

static void read_stats(long *sgrant, long *syield, long *sexpir, long *srevok, long *sabort) {
    FILE *f = fopen("/sys/kernel/debug/rseq/stats", "r");
    if (!f) { *sgrant = *syield = *sexpir = *srevok = *sabort = -1; return; }

    char line[256];
    *sgrant = *syield = *sexpir = *srevok = *sabort = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "sgrant:", 7) == 0) sscanf(line + 7, "%ld", sgrant);
        else if (strncmp(line, "syield:", 7) == 0) sscanf(line + 7, "%ld", syield);
        else if (strncmp(line, "sexpir:", 7) == 0) sscanf(line + 7, "%ld", sexpir);
        else if (strncmp(line, "srevok:", 7) == 0) sscanf(line + 7, "%ld", srevok);
        else if (strncmp(line, "sabort:", 7) == 0) sscanf(line + 7, "%ld", sabort);
    }
    fclose(f);
}

/* CPU stress child */
void stress_child(void) {
    volatile unsigned long x = 0;
    while (1) {
        for (int i = 0; i < 1000000; i++) x++;
    }
}

int main(void) {
    struct rseq *r = get_rseq();
    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    printf("=== Correct Yield Model Test ===\n");
    printf("Model: Grant happens on interrupt return, task keeps running,\n");
    printf("       task polls granted WHILE doing work, yields when seen.\n\n");

    // Enable extension
    prctl(PR_RSEQ_SLICE_EXTENSION, PR_RSEQ_SLICE_EXTENSION_SET, PR_RSEQ_SLICE_EXT_ENABLE, 0, 0);

    // Fork stress children for CPU pressure (interrupts will happen)
    printf("Spawning %d stress children for CPU pressure...\n", num_cpus);
    pid_t children[32];
    for (int i = 0; i < num_cpus && i < 32; i++) {
        children[i] = fork();
        if (children[i] == 0) {
            stress_child();
            exit(0);
        }
    }

    sleep(1);

    long g0, y0, e0, r0, a0;
    read_stats(&g0, &y0, &e0, &r0, &a0);
    printf("Stats before: grant=%ld yield=%ld expir=%ld revok=%ld abort=%ld\n\n", g0, y0, e0, r0, a0);

    int yields_ok = 0;
    int grants_seen = 0;
    int iterations = 0;

    printf("Running test: continuous work loop, polling for grants...\n");

    time_t start = time(NULL);
    while (time(NULL) - start < 5) {
        // Enter critical section
        r->slice_ctrl.request = 1;
        __asm__ __volatile__("" ::: "memory");

        // Do work in small chunks, polling for grant between chunks
        // This simulates real work where we might get interrupted
        for (int chunk = 0; chunk < 1000; chunk++) {
            // Small work chunk
            volatile unsigned long work = 0;
            for (int w = 0; w < 500; w++) work++;

            // Poll for grant - if we got one, yield immediately
            __asm__ __volatile__("" ::: "memory");
            if (r->slice_ctrl.granted) {
                grants_seen++;
                // Immediately yield - we're still running with the grant!
                long ret = syscall(__NR_rseq_slice_yield);
                if (ret == 1) {
                    yields_ok++;
                }
                r->slice_ctrl.granted = 0;
                // Continue work or break - let's continue to show we can
            }
        }

        // Exit critical section
        r->slice_ctrl.request = 0;
        iterations++;

        // Brief yield to let stress children run and create pressure
        sched_yield();
    }

    printf("Test done. Killing stress children...\n");
    for (int i = 0; i < num_cpus && i < 32; i++) {
        kill(children[i], 9);
        waitpid(children[i], NULL, 0);
    }

    long g1, y1, e1, r1, a1;
    read_stats(&g1, &y1, &e1, &r1, &a1);

    printf("\n=== RESULTS ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Grants seen in userspace: %d\n", grants_seen);
    printf("Yields returned 1: %d\n", yields_ok);
    printf("\nKernel stats delta:\n");
    printf("  sgrant: +%ld\n", g1 - g0);
    printf("  syield: +%ld\n", y1 - y0);
    printf("  sexpir: +%ld\n", e1 - e0);
    printf("  srevok: +%ld\n", r1 - r0);
    printf("  sabort: +%ld\n", a1 - a0);

    if (y1 - y0 > 0 || yields_ok > 0) {
        printf("\n*** SUCCESS! Yields are working! ***\n");
    } else if (g1 - g0 > 0) {
        printf("\n*** Grants working but no yields - sched_switch race? ***\n");
    } else {
        printf("\n*** No grants observed - need more CPU pressure? ***\n");
    }

    return 0;
}
