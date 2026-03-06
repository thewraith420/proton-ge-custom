# CRITICAL FIX: Wrong RSEQ Slice Yield Syscall Number

**Date**: 2026-02-22
**From**: BobZKernel workspace
**Priority**: CRITICAL - This prevents yields from working entirely

---

## The Problem

ProtonGE-RSEQ is calling the **WRONG syscall number** for `rseq_slice_yield`.

**Current (WRONG)**:
```c
// wine/dlls/ntdll/unix/sync.c line 85
#define __NR_rseq_slice_yield 470
```

**Kernel actual**:
```
arch/x86/entry/syscalls/syscall_64.tbl line 398:
471	common	rseq_slice_yield	sys_rseq_slice_yield
```

## Impact

- **Syscall 470** = `listns` (namespace listing syscall)
- **Syscall 471** = `rseq_slice_yield` (RSEQ yield syscall)

ProtonGE has been calling `listns` instead of `rseq_slice_yield` this entire time!

This is why:
- `syield` stat counter is always 0
- No yield messages appear in kernel debug logs
- Yields have never worked despite correct implementation

## The Fix

**File**: `/home/bob/buildstuff/proton-ge-rseq/wine/dlls/ntdll/unix/sync.c`

**Line 85**: Change from:
```c
#ifndef __NR_rseq_slice_yield
#define __NR_rseq_slice_yield 470  // ← WRONG!
#endif
```

To:
```c
#ifndef __NR_rseq_slice_yield
#define __NR_rseq_slice_yield 471  // ← CORRECT!
#endif
```

## After Fixing

1. Rebuild ProtonGE-RSEQ
2. Reinstall/deploy the fixed version
3. Run ESO or test program
4. Check kernel stats: `sudo cat /sys/kernel/debug/rseq/stats`
5. You should now see `syield` incrementing!

## How This Was Discovered

BobZKernel workspace added extensive debug logging to track yield syscalls:
- Added `RSEQ_YIELD_ENTER` messages to see when yields are called
- Added `RSEQ_YIELD_BLOCKED` to see when they fail
- No messages ever appeared despite ProtonGE code calling the syscall

Checked the ProtonGE source and found syscall number mismatch:
- ProtonGE: 470
- Kernel: 471

Verified kernel syscall table:
```bash
grep rseq_slice_yield arch/x86/entry/syscalls/syscall_64.tbl
# Returns: 471	common	rseq_slice_yield	sys_rseq_slice_yield
```

## Expected Results After Fix

With the correct syscall number, you should see:
- `syield` counter incrementing in `/sys/kernel/debug/rseq/stats`
- Kernel debug messages (if debug kernel is running):
  - `RSEQ_YIELD_ENTER` when yields are called
  - `RSEQ_YIELD_WILL_SET` when they succeed (if sched_switch=false)
  - `RSEQ_YIELD_BLOCKED` when blocked by sched_switch=true

## Related Kernel Work

BobZKernel workspace also discovered and fixed a kernel bug:

**Bug**: `rseq_irqentry_exit_to_user_mode()` was clearing ALL events including `sched_switch`:
```c
// WRONG - clears sched_switch prematurely
ev->events = 0;
```

**Fix**: Only clear user_irq and ids_changed, preserve sched_switch:
```c
// CORRECT - preserves sched_switch for slowpath
ev->user_irq = 0;
ev->ids_changed = 0;
```

This fix is already applied in BobZKernel 6.19.3-BobZKernel+.

## Testing Procedure

1. Apply the syscall number fix (470 → 471)
2. Rebuild ProtonGE: `cd /home/bob/buildstuff/proton-ge-rseq && ./build-proton.sh`
3. Install/deploy the new build
4. Clear kernel log: `sudo dmesg -C`
5. Run ESO or test program
6. Check stats: `sudo cat /sys/kernel/debug/rseq/stats`
7. Check logs: `sudo dmesg | grep RSEQ_YIELD`

Expected output:
```
syield: 123  (should be > 0 now!)
```

## Why This Matters

Even though grants are working and providing performance benefits, yields allow userspace to:
1. **Politely return unused timeslice** when work completes early
2. **Signal intent to reschedule** without blocking syscalls
3. **Cooperatively yield** for better scheduling decisions

Without working yields:
- Tasks hold grants until revoked by scheduler
- More overhead from forced revocations
- Less efficient resource sharing

With working yields:
- Tasks can release grants proactively
- Better CPU utilization
- Lower latency for other tasks

## Summary

**One-character fix** (470 → 471) will make yields work after months of investigation!

This was the missing piece - not a kernel bug, not a timing issue, not a race condition. Just a simple off-by-one error in the syscall number definition.

---

**Action Required**: Update syscall number in ProtonGE-RSEQ and test!
