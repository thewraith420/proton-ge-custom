# RSEQ Yield Timing Issue - Reactive vs Proactive

**Date**: 2026-02-22
**From**: BobZKernel workspace
**Issue**: Yields are never being called because grants are revoked before userspace can react

---

## Current ProtonGE Implementation

```c
/* Exit critical section */
rseq->slice_ctrl.request = 0;

/* If we were granted extended time, politely yield it back */
if (rseq->slice_ctrl.granted) {  // ← CHECK HERE
    rseq->slice_ctrl.granted = 0;
    syscall(__NR_rseq_slice_yield);  // ← NEVER REACHED
}
```

## The Problem: Race Condition

**Kernel Stats Show**:
```
sgrant: 282  (grants issued)
srevok: 282  (all 282 grants immediately revoked)
syield: 0    (yield syscall never called)
```

**Timeline**:
```
1. Userspace: request=1 (request slice)
2. Kernel: granted=1 (grant issued in exit-to-user path)
3. Kernel: IMMEDIATELY revoked (work_pending or interrupt)
4. Kernel: granted=0 (clears grant in userspace memory)
5. Userspace: checks granted → sees 0
6. Userspace: skips syscall
```

**Result**: The `if (rseq->slice_ctrl.granted)` check always sees 0, so yield is never called.

## Why Grants Are Immediately Revoked

Kernel stats show **100% revocation rate** (sgrant == srevok). This happens because:

1. **High scheduler pressure** - ESO has 80+ threads competing for CPU
2. **Constant interrupts** - Timer ticks, I/O, signals
3. **Work always pending** - By the time grant is issued, reschedule is already needed

The grant/revoke happens in the **same exit-to-user pass**:
```c
// Kernel exit-to-user path
if (work_pending || state.granted) {  // work_pending is true
    grant_slice();        // sgrant++
    revoke_immediately(); // srevok++
}
```

## Kernel Debug Results

**No yield messages seen**:
- `RSEQ_YIELD_ENTER` - Never printed (function not called)
- `RSEQ_YIELD_NO_GRANT` - Never printed (grant check passes in kernel)
- `RSEQ_YIELD_BLOCKED` - Never printed (sched_switch check never reached)

**Only sched_switch messages**:
- Thousands of `RSEQ_SCHED_SWITCH: setting sched_switch=true`
- Proves high scheduling pressure

## The Solution: Call Yield Unconditionally

The yield syscall is **safe to call even without a grant** - it just returns 0.

**Option 1: Always call yield** (RECOMMENDED)
```c
/* Exit critical section */
rseq->slice_ctrl.request = 0;

/* Always attempt yield - syscall returns 0 if no grant, 1 if yielded */
int yielded = syscall(__NR_rseq_slice_yield);

/* Optional: track successful yields */
// if (yielded) { /* we successfully yielded a grant */ }
```

**Option 2: Proactive yield during work**
```c
/* While doing work */
do_some_work();

/* Proactively yield periodically */
syscall(__NR_rseq_slice_yield);  // Yield if still granted

do_more_work();
```

**Option 3: Remove the check**
```c
/* Exit critical section */
rseq->slice_ctrl.request = 0;
rseq->slice_ctrl.granted = 0;  // Clear our copy
syscall(__NR_rseq_slice_yield);  // Always call
```

## Why This Matters

Even though yields won't succeed often (due to immediate revocation), calling the syscall:

1. **Provides visibility** - `syield` stat will show attempts
2. **Kernel can optimize** - Knowing about yield attempts helps scheduler
3. **Future-proof** - If grants last longer in future, yields will work
4. **Low overhead** - Syscall is very fast when no grant exists

## Expected Results After Fix

After removing the `if (granted)` check:

**Before**:
```
syield: 0    (never called)
```

**After**:
```
syield: 145  (called on every NtYieldExecution)
```

The actual successful yield count may still be low (because sched_switch prevents it), but at least the syscall will be invoked and tracked.

## Alternative: Accept Current Behavior

If you prefer to keep the check:

**Pros**:
- Avoids unnecessary syscalls when grant already revoked
- Slightly lower overhead

**Cons**:
- `syield` will always be 0 (no observability)
- Can't tell if yield mechanism is working
- Misses the rare cases where grant might still be active

## Recommendation

**Remove the `if (granted)` check** and always call the yield syscall.

The overhead is minimal (fast syscall), and it provides:
- Better observability (syield stat)
- Future compatibility (if grants last longer)
- Clearer code intent (always yield on exit)

---

## Files to Modify

**File**: `wine/dlls/ntdll/unix/sync.c`
**Function**: `rseq_critical_exit()`
**Lines**: ~145-148

**Change**:
```c
// BEFORE
if (rseq->slice_ctrl.granted) {
    rseq->slice_ctrl.granted = 0;
    syscall(__NR_rseq_slice_yield);
}

// AFTER
rseq->slice_ctrl.granted = 0;
syscall(__NR_rseq_slice_yield);
```

Simple 2-line removal!

---

**Status**: Waiting for ProtonGE workspace to apply fix and test
