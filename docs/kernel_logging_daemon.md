# Kernel Logging Daemon (logd)

## Overview

The kernel logging daemon (`/logd`) is a kernel task responsible for draining
per-core in-memory log ring buffers and writing their contents to the serial
port (COM1) in chronological order. It runs as a preemptible kernel thread,
which means the scheduler can interrupt it at any time and resume it later —
no special care is needed to yield the CPU voluntarily.

## Architecture

### Per-Core Ring Buffers

Each CPU core has its own `log_buffer_t` ring buffer (1 MB, defined by
`LOG_BUFFER_SIZE`). Log entries are written into the current core's buffer by
`log_store_entry()`, which is called from `printd()` in `serial_logging.c`.

Keeping one buffer per core avoids cross-core locking on the hot write path:
only the owning core ever advances `head`, and only logd ever advances `tail`.

### Log Entry Fields (`log_entry_t`)

| Field       | Type       | Description |
|-------------|------------|-------------|
| `ticks`     | `uint64_t` | `kTicksSinceStart` at the time `printd()` was called. Used in the display prefix. |
| `tsc`       | `uint64_t` | `RDTSC` captured inside `log_store_entry()`. Used for cross-core sort ordering. |
| `core_id`   | `uint16_t` | APIC ID of the core that produced this entry. |
| `log_level` | `uint8_t`  | Priority extracted from the `debug_level` bitmask. |
| `category`  | `uint8_t`  | Category index (first set bit of `debug_level`). |
| `threadID`  | `uint64_t` | Thread ID of the running thread at log time. |
| `continued` | `bool`     | True if this entry is a continuation chunk of a multi-chunk message. |
| `message`   | `char[]`   | The formatted message payload (max `MAX_LOG_MESSAGE_SIZE` bytes). |

Long messages are split into multiple entries with `continued = true` on all
chunks after the first. The display prefix is only printed on the first chunk.

### Output Format

```
<ticks> (0x<threadID>) AP<core_id>: <message>
```

Example:
```
587 (0x0023) AP1: scheduler: switching to thread 0x0021
```

## How logd Works

### Wakeup Cycle

logd runs in a `while(1)` loop. After each drain pass it calls
`sigaction(SIGSLEEP, ...)` with a wakeup time of `kTicksSinceStart +
LOGD_SLEEP_TICKS` (currently 1 second). The scheduler moves it to the
interruptible sleep queue (`qISleep`) and wakes it when the tick count is
reached. logd then acquires the work lock and drains again.

logd sleeps for the same duration regardless of whether any logs were written
during the last wakeup. The sleep period is a backoff, not a busy-poll.

### Work Lock

`kLogDWorkLock` is a TAS spinlock. Only one CPU may be draining buffers at a
time. `panic()` calls `logd_thread(false)` synchronously to flush buffered
messages before halting; the lock prevents a concurrent daemon run from
interleaving with the panic output.

If the lock is already held when logd wakes (rare), it skips the work and goes
back to sleep.

### Buffer-Full Handling

If a core's ring buffer fills up (`head + 1 == tail`), `log_store_entry()`
calls `logd_thread(false)` inline to drain synchronously. If that still does
not free space, it panics. This is a last resort; in normal operation the 1 MB
buffer is large enough that logd drains it long before it fills.

## Drain Algorithm: k-Way TSC Merge

The drain loop performs a **k-way merge** across all core buffers, sorted by
`tsc`. On each step:

1. Scan all `kMPCoreCount` buffers; find the non-empty one whose oldest entry
   has the smallest `tsc` value.
2. Call `logd_drain_one()` on that buffer, which prints the entry and advances
   `tail`.
3. `logd_drain_one()` immediately drains any following `continued` entries from
   the same buffer before returning, keeping multi-chunk messages intact.
4. Repeat until all buffers are empty.

This produces output that is globally sorted by TSC rather than grouped by
core, so interleaved SMP activity appears in true chronological order.

**Complexity:** O(N × k) time, O(1) extra space, where N is the total number
of entries and k is the number of cores. For k ≤ 8 this is faster than
sorting a copied array (O(N log N)) and requires no allocation.

### TSC Accuracy Note

On QEMU, all vCPUs share a single host TSC source, so cross-core TSC
comparisons are valid. On real hardware this requires an invariant TSC
(CPUID leaf 0x80000007, EDX bit 8). Without it, per-core TSC values may drift
and sort order within the same tick could be wrong, though the output would
still be grouped correctly at tick granularity.

## Configuration (`log.h`)

| Constant          | Default  | Meaning |
|-------------------|----------|---------|
| `LOG_BUFFER_SIZE` | 1 MB     | Ring buffer size per core |
| `MAX_LOG_MESSAGE_SIZE` | 256 | Maximum bytes per message chunk |
| `LOGD_SLEEP_TICKS` | 100 ticks (1 s) | How long logd sleeps between drain passes |

## Enabling / Disabling

Log buffering is controlled by `ENABLE_LOG_BUFFERING` in `kernel/include/CONFIG.h`:

- `1` — buffered mode: all `printd()` output goes through the ring buffers and
  is drained by logd. Serial output is ordered by TSC.
- `0` — direct mode: `printd()` writes straight to the serial port without
  buffering. Output order is first-come-first-served; SMP output interleaves
  at character granularity.

---

## Changelog

### Initial implementation (before this session)

- `ENABLE_LOG_BUFFERING` existed but was set to `0` (disabled). `printd()`
  wrote directly to the serial port; concurrent SMP output from AP0 and AP1
  interleaved at byte level, producing garbled lines.

### Stable ring-buffer logging (`f190fa4`)

- Enabled `ENABLE_LOG_BUFFERING = 1`. The OS can now run indefinitely with
  logd active without hanging or corrupting the serial stream.
- `logd_thread()` was a simple nested loop: for each core 0→N, drain up to
  `MAX_BATCH_SIZE = 1000` entries in core order, then sleep. This meant AP0's
  entries always appeared before AP1's entries regardless of when they were
  produced.
- `panic()` now calls `logd_thread(false)` before halting so buffered log
  entries are flushed and visible in the log file.
- Fixed `logd_thread` non-daemon return: `nonDaemonRunSuccess` was set to
  `true` before doing any work. Changed to `processed_logs > 0` so callers
  can tell whether anything was actually flushed.

### TSC-sorted k-way merge drain (this session)

**Struct changes (`log_entry_t`):**
- Removed `tick_count` — was set to `kTicksSinceStart` at `printd()` call
  time but never read anywhere. Dead field.
- Renamed `timestamp` → `ticks` — clearer name for what it actually holds
  (`kTicksSinceStart` at `printd()` call time). Still used for the display
  prefix.
- Added `tsc` — `RDTSC` captured inside `log_store_entry()` at the moment
  the entry is written to the ring buffer. Used exclusively for sort ordering.

**Algorithm changes (`logd_thread`):**
- Replaced the per-core sequential drain + `MAX_BATCH_SIZE` limit with a
  k-way TSC merge that drains **all** cores to empty before sleeping.
  Removing the batch cap is safe because the scheduler's preemption timer
  handles logd's CPU share; an artificial limit just caused the last entries
  in a busy second to wait a full sleep period unnecessarily.
- Extracted `logd_drain_one()` helper that prints one entry and immediately
  drains any following `continued` chunks from the same buffer, preventing
  multi-chunk messages from being split across entries from other cores.
- Removed `MAX_BATCH_SIZE` constant from `log.h` (no longer referenced).
