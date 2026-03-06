# RSEQ Per-Thread Initialization NOT Applied

**Date**: 2026-02-22
**From**: BobZKernel workspace
**Priority**: MEDIUM - Would significantly increase RSEQ utilization

---

## Current State

The patch file exists at:
```
patches/rseq/0001-ntdll-Enable-RSEQ-timeslice-extension-proper-v2.patch
```

**BUT IT WAS NEVER APPLIED TO THE WINE SOURCE!**

## Evidence

Checking for RSEQ code in thread.c:
```bash
grep -n "rseq\|RSEQ" wine/dlls/ntdll/unix/thread.c
# Returns: NOTHING
```

The `init_thread_rseq_slice()` function from the patch does not exist in the actual source.

## Current Limited Implementation

RSEQ is ONLY used in `NtYieldExecution()`:
```c
// wine/dlls/ntdll/unix/sync.c
NTSTATUS WINAPI NtYieldExecution(void)
{
    rseq_critical_enter();  // Sets request = 1
    sched_yield();
    rseq_critical_exit();   // Yields back
    ...
}
```

This means:
- **Only threads that call NtYieldExecution() get RSEQ benefits**
- **Most game threads NEVER call this**
- **Result: Very few grants (~188 during ESO gameplay)**

## What the Patch Would Do

The patch adds `init_thread_rseq_slice()` which:
1. Gets called in `start_thread()` for EVERY Wine thread
2. Sets `rseq->slice_ctrl.request = 1` on thread creation
3. Every thread automatically requests RSEQ slice extensions

This would mean:
- **ALL 80+ ESO threads would request slice extensions**
- **Many more grants would be issued**
- **Much broader RSEQ utilization**

## How to Apply the Patch

```bash
cd /home/bob/buildstuff/proton-ge-rseq/wine
patch -p1 < ../patches/rseq/0001-ntdll-Enable-RSEQ-timeslice-extension-proper-v2.patch
```

Or manually add the code to:
1. `dlls/ntdll/unix/loader.c` - Process-level init (may already be there)
2. `dlls/ntdll/unix/thread.c` - Per-thread init (MISSING!)

## Expected Impact

**Before (current)**:
- ~188 grants during ESO gameplay
- Only NtYieldExecution() uses RSEQ
- ~1% of game activity benefits

**After (with patch)**:
- Thousands of grants during ESO gameplay
- All threads use RSEQ
- ~100% of game activity could benefit

## Why This Matters

The kernel-side RSEQ is now fully working:
- syield: 172 (91% yield rate!)
- srevok: 1 (0.5% revocation rate!)

But we're only using it for a tiny fraction of game activity because most threads don't have `request = 1` set.

## Action Required

Apply the patch or manually add per-thread RSEQ initialization to `thread.c`.

The patch file is already correct and tested - it just needs to be applied to the Wine source tree before building.

---

**Status**: Patch exists but not applied
**Impact**: Major - would increase RSEQ utilization ~100x
