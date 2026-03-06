# RSEQ Abort Syscall Tracking - Now Available!

**Date:** February 27, 2026
**Status:** 🎉 **IMPLEMENTED** - Kernel now tracks which syscalls cause aborts
**Kernel Version:** 6.19.3-BobZKernel+ build #3

## Breakthrough!

The BobZKernel now has **per-syscall abort tracking** implemented! This gives us the data we need to optimize yield placement in ProtonGE.

## What Was Added

### Enhanced RSEQ Statistics

The kernel now tracks which syscalls are causing RSEQ grant aborts and displays a breakdown with percentages:

**Stats location:** `/sys/kernel/debug/rseq/stats`

**New output format:**
```
exit:          488488709
signal:         12599809
slowp:              3744
fastp:           5626030
ids:             5400181
cs:                    0
clear:                 0
fixup:                 0
sgrant:            13089
sexpir:             4790
srevok:             6740
syield:              233
sabort:             6116

Abort breakdown:
sabort_futex:          3500  (57%)
sabort_ioctl:          1200  (20%)
sabort_read:            800  (13%)
sabort_write:           400  (7%)
sabort_poll:            150  (2%)
sabort_epoll:            40  (1%)
sabort_recv:             15  (0%)
sabort_send:              5  (0%)
sabort_select:            3  (0%)
sabort_sleep:             2  (0%)
sabort_other:             1  (0%)
```

### Syscalls Being Tracked

The kernel now tracks aborts from:

1. **futex (202)** - Thread synchronization, mutex/semaphore operations
2. **read (0)** - File/socket reads
3. **write (1)** - File/socket writes
4. **poll (7)** - I/O polling
5. **epoll_wait (232), epoll_pwait (281)** - Event polling
6. **ioctl (16)** - Device control (GPU driver calls!)
7. **recvfrom (45), recvmsg (47)** - Network receive
8. **sendto (44), sendmsg (46)** - Network send
9. **select (23), pselect6 (270)** - I/O multiplexing
10. **nanosleep (35), clock_nanosleep (230)** - Sleep operations
11. **other** - All other syscalls

## How to Use This Data

### Step 1: Collect Abort Data

Run ESO for 5-10 minutes with ProtonGE-RSEQ, then check:

```bash
sudo cat /sys/kernel/debug/rseq/stats
```

### Step 2: Identify Top Abort Sources

Look at the abort breakdown percentages. For example:

- **60% futex** → Focus on `futex_wake_one()` and mutex operations
- **20% ioctl** → GPU driver interaction (may be unavoidable)
- **10% read/write** → File I/O during critical sections
- **5% poll/epoll** → I/O event handling

### Step 3: Prioritize Yield Placement

Add yields where aborts are highest:

#### If futex dominates (expected):
```c
// wine/dlls/ntdll/unix/sync.c
static inline int futex_wake_one( const LONG *addr )
{
    int ret = syscall( __NR_futex, addr, FUTEX_WAKE_PRIVATE, 1, NULL, 0, 0 );
    syscall(__NR_rseq_slice_yield);  // ← ADD THIS
    return ret;
}

NTSTATUS WINAPI NtReleaseMutant( HANDLE handle, LONG *prev_count )
{
    // ... existing code ...

    syscall(__NR_rseq_slice_yield);  // ← ADD THIS
    return STATUS_SUCCESS;
}
```

#### If ioctl is high:
This is likely GPU driver calls - probably unavoidable, but we could try yielding after DirectX/Vulkan calls in DXVK layer.

#### If read/write is high:
File I/O during critical sections - consider yielding after file operations:
```c
// After any read/write operations
syscall(__NR_rseq_slice_yield);
```

### Step 4: Test and Measure

After adding yields:
1. Rebuild ProtonGE
2. Run ESO for 5-10 minutes
3. Check stats again
4. **Expected:** Corresponding abort category should decrease, yield rate should increase

## Example Analysis Workflow

### Before Optimization
```bash
$ sudo cat /sys/kernel/debug/rseq/stats
...
sabort: 6116

Abort breakdown:
sabort_futex:   3500  (57%)  ← HIGH - needs yields after futex ops
sabort_ioctl:   1200  (20%)  ← GPU driver - may be unavoidable
sabort_read:     800  (13%)  ← File I/O - add yields after reads
sabort_write:    400  (7%)   ← File I/O - add yields after writes
sabort_other:    216  (3%)
```

**Action:** Add yields to:
1. `futex_wake_one()` (highest priority)
2. `NtReleaseMutant()`, `NtReleaseSemaphore()`, `NtSetEvent()`
3. After file read/write operations

### After Optimization (Expected)
```bash
$ sudo cat /sys/kernel/debug/rseq/stats
...
sabort: 2500  ← Total aborts reduced
syield: 4800  ← Yields increased!

Abort breakdown:
sabort_futex:    500  (20%)  ← Reduced from 57%!
sabort_ioctl:   1200  (48%)  ← Same (unavoidable)
sabort_read:     400  (16%)  ← Reduced from 13%
sabort_write:    200  (8%)   ← Reduced from 7%
sabort_other:    200  (8%)
```

**Result:** Futex aborts drop from 3500 to 500 because we added yields!

## Integration with Existing Work

This complements our previous findings:

### Previous Discovery (RSEQ-YIELD-COVERAGE-ISSUE.md)
- Problem: Only `NtYieldExecution()` calls yield
- Impact: 0-2% yield rate, 51-60% revocation rate
- Solution: Add yields after synchronization primitives

### New Capability (This Feature)
- **Data-driven optimization:** Know exactly which syscalls to target
- **Measure impact:** See if yields reduce specific abort categories
- **Prioritize work:** Focus on high-percentage abort sources first

## Technical Implementation Details

### Kernel Changes

**Files modified:**
1. `include/linux/rseq_entry.h` - Added per-syscall counters to `struct rseq_stats`
2. `kernel/rseq.c` - Added tracking logic and output formatting

**Key code:**
```c
// In rseq_syscall_enter_work() when abort occurs:
switch (syscall) {
case 202: /* __NR_futex */
    rseq_stat_inc(rseq_stats.s_abort_futex);
    break;
// ... etc for each tracked syscall
default:
    rseq_stat_inc(rseq_stats.s_abort_other);
    break;
}
```

**Performance impact:** Negligible (single switch statement per abort)

## Next Steps for ProtonGE Optimization

### Phase 1: Data Collection (NOW)
1. ✅ Kernel feature implemented
2. ⏳ Run ESO gaming sessions
3. ⏳ Collect abort breakdown data
4. ⏳ Identify top 2-3 abort sources

### Phase 2: Targeted Yield Implementation
Based on data, add yields to highest-impact locations:
- If `sabort_futex` is high → Add to futex operations
- If `sabort_ioctl` is high → Consider DXVK-level yields
- If `sabort_read/write` is high → Add to file I/O paths

### Phase 3: Validation
1. Rebuild ProtonGE with yields
2. Re-run gaming sessions
3. Compare before/after abort breakdown
4. Measure yield rate improvement

### Phase 4: Iteration
Continue adding yields to remaining high-abort sources until:
- Yield rate reaches 40-60%
- Revocation rate drops below 30%
- Abort rate minimized (unavoidable aborts only)

## Success Metrics

**Current State:**
- Yield rate: 0-2%
- Revocation rate: 51-60%
- Abort rate: 47-58%
- **No data on which syscalls cause aborts**

**Target State:**
- Yield rate: 40-60%
- Revocation rate: 20-30%
- Abort rate: 20-30% (from unavoidable syscalls like GPU ioctl)
- **Data-driven yield placement based on abort breakdown**

## Documentation References

**Related documents in this workspace:**
- `RSEQ-YIELD-COVERAGE-ISSUE.md` - Why yields are needed
- `RSEQ-PER-THREAD-INIT-MISSING.md` - Grant initialization fix
- `RSEQ_SYSCALL_NUMBER_FIX.md` - Syscall number correction
- `RSEQ-YIELD-TIMING-ISSUE.md` - Reactive vs proactive yields

**BobZKernel references:**
- Implementation plan: `/home/bob/buildstuff/BobZKernel/CONTEXT/RSEQ-ABORT-TRACKING-IMPLEMENTATION.md`
- Stats location: `/sys/kernel/debug/rseq/stats`
- Kernel source: `builds/linux-6.19/kernel/rseq.c`

## Quick Reference Commands

**View stats:**
```bash
sudo cat /sys/kernel/debug/rseq/stats
```

**Monitor during gaming:**
```bash
/home/bob/buildstuff/BobZKernel/scripts/monitor-rseq-stats.sh 300
```

**Analyze interrupts:**
```bash
/home/bob/buildstuff/BobZKernel/scripts/analyze-rseq-interrupts.sh
```

## Example: Interpreting Results

**Scenario: futex dominates aborts**

```
Abort breakdown:
sabort_futex:   4200  (68%)  ← CRITICAL - add yields to futex ops
sabort_ioctl:    800  (13%)  ← GPU driver - unavoidable
sabort_read:     600  (10%)  ← Medium priority
sabort_write:    300  (5%)   ← Low priority
sabort_other:    250  (4%)
```

**Action plan:**
1. **Immediate:** Add `syscall(__NR_rseq_slice_yield)` after `futex_wake_one()`
2. **High priority:** Add yields to `NtReleaseMutant/Semaphore/SetEvent`
3. **Medium priority:** Add yields after critical file reads
4. **Low priority:** Other optimizations

**Expected impact:** Reduce 68% futex aborts to <20%, increase yield rate from 2% to 40%+

---

## Conclusion

This is the missing piece we needed! Instead of blindly adding yields everywhere, we can now:

✅ **Measure** which syscalls cause the most aborts
✅ **Prioritize** yield placement based on real data
✅ **Validate** that our changes reduce the targeted abort categories
✅ **Iterate** efficiently on the highest-impact improvements

**The combination of:**
1. Per-thread RSEQ initialization (grants working)
2. Syscall work hook (yields working)
3. **Abort syscall tracking (data-driven optimization)** ← NEW!

**Gives us everything we need to fully optimize ProtonGE-RSEQ!**

Time to collect some data and see what's really causing those aborts! 🎯
