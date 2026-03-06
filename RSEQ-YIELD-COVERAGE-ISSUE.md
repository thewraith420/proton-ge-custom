# RSEQ Yield Coverage Issue - Critical Discovery

**Date:** February 22, 2026
**Status:** 🔴 **CRITICAL - Low Yield Rate Discovered**
**Impact:** RSEQ feature working but vastly underutilized

## Executive Summary

After implementing per-thread RSEQ initialization (which successfully increased grants from ~188 to 25,832), we discovered that **cooperative yields are barely happening** (~0-2% yield rate). This severely limits the effectiveness of RSEQ slice extensions.

**Root Cause:** RSEQ yield calls (`syscall(__NR_rseq_slice_yield)`) are only present in **ONE location** in the entire Wine codebase - `NtYieldExecution()` in sync.c.

**Current Performance:**
- ✅ Grants working: 13,089 grants during gaming session
- ✅ Per-thread init working: All Wine threads requesting RSEQ
- ❌ Yields failing: Only 233 yields (2% yield rate)
- ❌ High revocation rate: 51-60% of grants forcefully revoked by scheduler

**Expected Performance:**
- Target yield rate: 40-60% (similar to expiration rate)
- Current yield rate: 0-2% (critically low)

## Technical Analysis

### What's Working

**Per-thread initialization (FIXED in previous session):**
```c
// wine/dlls/ntdll/unix/thread.c:1206
static void init_thread_rseq_slice(void)
{
    // Called on every thread creation
    prctl(PR_RSEQ_SLICE_EXTENSION_SET, PR_RSEQ_SLICE_EXT_ENABLE);
    // Sets request=1 permanently for this thread
}
```
**Result:** All Wine threads can receive RSEQ grants ✅

### What's Broken

**Yield coverage (CURRENT ISSUE):**
```c
// wine/dlls/ntdll/unix/sync.c:134-147
static inline void rseq_critical_exit(void)
{
    // ... validation code ...
    syscall(__NR_rseq_slice_yield);  // ← ONLY yield call in entire codebase!
}
```

**Current usage:**
```c
// wine/dlls/ntdll/unix/sync.c:2828-2830 (NtYieldExecution)
rseq_critical_enter();  // Request grant
// ... minimal work ...
rseq_critical_exit();   // Yield
```

**Problem:** Only threads explicitly calling `NtYieldExecution()` ever yield. This is rarely called!

## Comparison: Grants vs Yields

### Grants (Working Well)
**Before per-thread init:**
- Only threads calling `NtYieldExecution()` requested grants
- Result: ~188 grants during gaming

**After per-thread init:**
- ALL Wine threads request grants automatically
- Result: 25,832 grants during gaming (137x improvement!)

### Yields (Broken)
**Current state:**
- Only `NtYieldExecution()` calls yield
- Result: 233 yields during gaming (~0-2% yield rate)

**What we need:**
- ALL synchronization primitives should yield after completion
- Expected: 40-60% yield rate (thousands of yields)

## Where Yields Should Be Added

Based on Wine's synchronization primitives in `sync.c`, yield calls should be added after:

### 1. **Futex Operations**
```c
// After futex_wake_one() - waking another thread
static inline int futex_wake_one( const LONG *addr )
{
    int ret = syscall( __NR_futex, addr, FUTEX_WAKE_PRIVATE, 1, NULL, 0, 0 );
    syscall(__NR_rseq_slice_yield);  // ← ADD: Yield after waking another thread
    return ret;
}
```

### 2. **Mutex Release**
```c
NTSTATUS WINAPI NtReleaseMutant( HANDLE handle, LONG *prev_count )
{
    // ... existing code ...

    // After releasing mutex
    syscall(__NR_rseq_slice_yield);  // ← ADD: Yield so waiting thread can acquire

    return STATUS_SUCCESS;
}
```

### 3. **Semaphore Release**
```c
NTSTATUS WINAPI NtReleaseSemaphore( HANDLE handle, ULONG count, ULONG *previous )
{
    // ... existing code ...

    // After releasing semaphore
    syscall(__NR_rseq_slice_yield);  // ← ADD: Yield for waiting threads

    return STATUS_SUCCESS;
}
```

### 4. **Event Setting**
```c
NTSTATUS WINAPI NtSetEvent( HANDLE handle, LONG *prev_state )
{
    // ... existing code ...

    // After setting event
    syscall(__NR_rseq_slice_yield);  // ← ADD: Yield for threads waiting on event

    return STATUS_SUCCESS;
}
```

### 5. **Critical Section Exit**
```c
// In any RtlLeaveCriticalSection equivalent
void leave_critical_section(void)
{
    // ... existing code ...

    // After leaving critical section
    syscall(__NR_rseq_slice_yield);  // ← ADD: Yield after releasing lock
}
```

## Implementation Strategy

### Phase 1: High-Impact Locations (Immediate)
Add yields to the most frequently called synchronization primitives:
1. `futex_wake_one()` - Used constantly for thread wakeup
2. `NtReleaseMutant()` - Mutex release
3. `NtSetEvent()` - Event signaling

**Expected impact:** 20-40% yield rate improvement

### Phase 2: Comprehensive Coverage (Follow-up)
Add yields to all synchronization primitives:
1. `NtReleaseSemaphore()`
2. All lock release operations
3. Condition variable signals
4. Barrier completions

**Expected impact:** 40-60% yield rate (optimal)

### Phase 3: Fine-tuning (Optional)
Add conditional yields based on contention:
```c
if (likely_contended) {
    syscall(__NR_rseq_slice_yield);
}
```

## Code Pattern to Use

### Simple Pattern (Recommended for Phase 1)
```c
#ifdef HAVE_RSEQ_SLICE
    syscall(__NR_rseq_slice_yield);
#endif
```

### Conditional Pattern (For optimization)
```c
#ifdef HAVE_RSEQ_SLICE
    // Only yield if we likely woke another thread
    if (ret > 0) {  // futex_wake returned success
        syscall(__NR_rseq_slice_yield);
    }
#endif
```

## Performance Expectations

### Current Performance
```
Gaming Session (5 minutes):
  Grants: 13,089
  Yields: 233 (2%)
  Revokes: 6,740 (51%)
  Expires: 4,790 (37%)
```

### Expected After Fixes
```
Gaming Session (5 minutes):
  Grants: ~15,000 (similar)
  Yields: 6,000-9,000 (40-60%) ← Major improvement!
  Revokes: 3,000-4,500 (20-30%) ← Reduced
  Expires: 4,500-6,000 (30-40%) ← Similar
```

**Key improvements:**
- ✅ Yields increase from 2% to 40-60%
- ✅ Revocations decrease from 51% to 20-30%
- ✅ Better cooperation between threads
- ✅ Lower scheduler pressure
- ✅ Smoother gaming performance

## Why This Matters

### The RSEQ Contract
RSEQ slice extensions are designed for **cooperative multitasking**:

1. **Request grant** before critical section: "I need protection"
2. **Do critical work** with ~30µs protection from preemption
3. **Yield cooperatively** after completion: "I'm done, others can run"

**Currently we're doing:**
1. ✅ Request grant (working - all threads)
2. ✅ Do critical work (working - 37% complete naturally)
3. ❌ Yield cooperatively (BROKEN - only 2% yield!)

Without step 3, we're forcing the **kernel to revoke grants** (51% revocation rate) instead of yielding cooperatively. This defeats the purpose!

### Analogy
Imagine a library with time-limited study rooms:

**Current implementation:**
- Everyone requests a room ✅
- Most people use the room productively ✅
- Almost nobody tells the desk when they're done ❌
- Security guard has to kick people out (revocations) ❌

**Correct implementation:**
- Everyone requests a room ✅
- Most people use the room productively ✅
- Everyone tells the desk when finished ✅
- Next person can immediately use the room ✅

## Testing Plan

### 1. Add yields to futex_wake_one (Quick Test)
**File:** `wine/dlls/ntdll/unix/sync.c`
**Lines:** ~203-206

```diff
 static inline int futex_wake_one( const LONG *addr )
 {
-    return syscall( __NR_futex, addr, FUTEX_WAKE_PRIVATE, 1, NULL, 0, 0 );
+    int ret = syscall( __NR_futex, addr, FUTEX_WAKE_PRIVATE, 1, NULL, 0, 0 );
+#ifdef HAVE_RSEQ_SLICE
+    syscall(__NR_rseq_slice_yield);  // Yield after waking another thread
+#endif
+    return ret;
 }
```

**Test:**
- Rebuild ProtonGE
- Run ESO for 5 minutes
- Check RSEQ stats: `sudo cat /sys/kernel/debug/rseq/stats`
- **Expected:** Yield rate increases from 2% to 10-20%

### 2. Add yields to all sync primitives (Full Fix)
Apply to:
- `NtReleaseMutant()`
- `NtReleaseSemaphore()`
- `NtSetEvent()`
- All critical section exits

**Test:**
- Rebuild ProtonGE
- Run ESO for 5 minutes
- **Expected:** Yield rate reaches 40-60%

### 3. Monitor with BobZKernel monitoring script
```bash
/home/bob/buildstuff/BobZKernel/scripts/monitor-rseq-stats.sh 300
```

## Critical Files to Modify

**Primary file:**
- `wine/dlls/ntdll/unix/sync.c` - All synchronization primitives

**Secondary files (if needed):**
- `wine/dlls/ntdll/unix/thread.c` - Thread synchronization
- Any file with pthread mutex operations

## Success Criteria

### Minimum Success (Phase 1)
- ✅ Yield rate increases to 20%+
- ✅ Revocation rate decreases below 45%
- ✅ No performance regressions

### Good Success (Phase 2)
- ✅ Yield rate reaches 40-50%
- ✅ Revocation rate drops to 25-35%
- ✅ Noticeable smoothness improvement

### Excellent Success (Optimal)
- ✅ Yield rate 50-60%
- ✅ Revocation rate below 25%
- ✅ Yields > Revokes (the holy grail!)

## Next Steps

1. **Immediate:** Add yield to `futex_wake_one()` (single line change)
2. **Short-term:** Add yields to `NtReleaseMutant/Semaphore/SetEvent`
3. **Long-term:** Comprehensive yield coverage audit
4. **Testing:** Use monitoring script to validate improvements

## References

- **Kernel workspace:** `/home/bob/buildstuff/BobZKernel/`
- **Kernel RSEQ patch:** `patches/cachyos-6.19/9002-rseq-slice-extension.patch`
- **Kernel stats:** `/sys/kernel/debug/rseq/stats`
- **Monitoring script:** `BobZKernel/scripts/monitor-rseq-stats.sh`
- **Previous fix:** `RSEQ-PER-THREAD-INIT-MISSING.md` (grant initialization)

## Appendix: Performance Data

### Session 1: With All Kernel Fixes + Per-thread Init
```
Duration: 5 minutes
Grants: 25,832
Yields: 1,342 (5%)
Revokes: 9,389 (36%)
Expires: 7,425 (29%)
Result: Good grant coverage, low yield rate
```

### Session 2: Fresh Boot, Same Configuration
```
Duration: 5 minutes
Grants: 13,089
Yields: 233 (2%)
Revokes: 6,740 (51%)
Expires: 4,790 (37%)
Result: Yield rate even lower, revocations higher
```

### Session 3: Live Monitoring (Most Recent)
```
Duration: ~3.5 minutes (partial)
Grants: 8,085
Yields: 52 (0%)
Revokes: 4,910 (60%)
Expires: 3,769 (46%)
Result: Yields virtually non-existent, 60% revocation rate
```

**Conclusion:** The low yield rate is consistent and severe across all sessions. This is NOT a kernel issue - it's a Wine/ProtonGE implementation gap that must be addressed.

---

**End of Report**

*This issue prevents RSEQ from reaching its full potential. Fixing yield coverage should be the #1 priority for ProtonGE-RSEQ development.*
