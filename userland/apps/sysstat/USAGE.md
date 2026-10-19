# sysstat

`sysstat` collects a finite series of performance samples and exits. It uses
the `libos64` monitor shared by `top` and `htop`, and works through an SSH
command without a terminal.

```sh
sysstat --count 10 --interval-ms 1000
sysstat --count 20 --interval-ms 250 --cores --top 5
sysstat --count 10 --interval-ms 1000 --tid 42 --network --cache --json
ssh os64@HOST 'sysstat -n 10 -i 1000 -j'
```

## Options

| Option | Meaning |
| --- | --- |
| `-n`, `--count N` | Number of reports, 1 through 4294967295; default 10 |
| `-i`, `--interval-ms N` | Requested sampling interval, 100 through 60000 milliseconds; default 1000 |
| `-p`, `--tid ID` | Select a task and report its thread activity; this is the task ID labeled TID by `top` |
| `-t`, `--top N` | Show up to N busiest live, non-idle tasks; 0 through 512, default 0 |
| `-c`, `--cores` | Include individual cores, with their actual core IDs and parked state |
| `-N`, `--network` | Include per-interface traffic and TCP counters |
| `-C`, `--cache` | Include block-cache occupancy and activity |
| `-j`, `--json` | Emit newline-delimited JSON instead of the text table |
| `-h`, `--help` | Show usage and exit |

An initial baseline is collected before waiting for the first report. The
baseline does not count toward `--count`. A 250 ms interval requests four
reports per second. Collection and output can delay delivery; the command
reports measured intervals and lateness. It skips elapsed deadlines after
slow collection or output, keeping its schedule anchored without producing
a burst of reports for missed intervals.

## Measurements

The default text table shows CPU percentages, used and available memory,
the change in used memory, the largest free memory extent, task and thread
counts, observed and parked cores, zombies, and sampling lateness. Text
memory sizes use powers of 1024. CPU columns are percentages; BUSY is user
plus system work, with scheduler time reported separately.
Text samples with optional core, task, network or cache detail are separated
by a blank line. The default compact table and JSON output have no separators.

`--cores` adds up to three side-by-side groups, read down each group before
moving to the next. Twelve cores occupy four data rows; 24 cores occupy eight.
The layout fits an 80-column console without querying a terminal; unusually
wide core IDs reduce the number of groups. A legend expands the compact
`CPU BUS USR SYS SCH IDL` headings. Values are percentages over the sampled
interval; `IDL` is idle time, not the name of the task running on that core.
Small amounts of activity
can round to 0% busy and 100% idle. Collection precedes output, so these
rows do not describe the instant when `sysstat` prints them.

Whole-machine CPU uses 100% for the machine's capacity. Task and thread CPU
use 100% for one core, so a multithreaded task can exceed 100%. Integer
percentages follow the shared monitor's rounding. It uses core 0's CPU
ledger for the CPU interval within its sanity window, otherwise the tick
clock; JSON exposes that interval alongside the independently measured
monotonic interval and the ledger/tick discrepancy.

Counts include kernel, idle and zombie tasks from the monitor snapshot.
The thread count is the sum of the task reports, rather than the number of
expanded worker rows. `--top` excludes idle tasks, zombies and tasks without
a valid CPU interval. Ties use ascending task ID.

`--tid` adds the selected task's CPU split, state, core, thread count, reported
heap size and interval counts of context switches and minor/major faults.
Heap size is the task's reported heap field, not resident memory; that reader
uses zero when a heap report cannot be read. Expanded
thread reports are collected for tasks with at least two threads. A task
that disappears is reported as unavailable while sampling continues.

`--network` adds each observed interface's name, link state, receive/transmit
bytes and frames per second, errors, and both receive-drop reasons. TCP
counters cover segments in/out, retransmissions, fast retransmissions, local
transmit drops, bad checksums, connection timeouts and received resets.
Traffic includes the SSH connection carrying these reports.

`--cache` adds block-cache enabled state, resident bytes, capacity, interval
hit percentage, and rates for hits, misses, fills, evictions, write updates,
edge bypasses and discarded racing fills. Hits and misses count request
segments; fills, evictions, updates and discarded races count cache-line
events. Edge bypasses count request segments served uncached. The hit
percentage is interval hits divided by interval hits plus misses; no accesses
means unavailable. These counters describe the shared block cache. The kernel
does not export per-device byte throughput or latency through this interface;
cache activity cannot measure all disk reads or writes. RAMDisk traffic does
not pass through this cache.

The optional I/O sampler lives in `libos64`, with independent history for each
consumer. It computes deltas and integer rates using each source's measured
monotonic interval, rounding rates down. Missing sources, a changed interface
identity (name plus MAC), decreasing counters, and a changed cache enabled
state start a fresh baseline. Failed clocks or nonpositive intervals suppress
rates; counter deltas can still be available. An unchanged counter produces
a valid zero rate. Overflow suppresses the affected source's rate tuple.

The final summary reports average and peak busy CPU, lowest available
memory, longest measured interval, maximum lateness, skipped deadlines and
partial samples. Average CPU is weighted by the monitor's CPU intervals and
truncated to an integer percentage. Missing measurements do not contribute
to the relevant average or minimum; sample counts indicate coverage.

## JSON contract

Each output line is a complete JSON object. Successful runs emit `count`
objects with `type: "sample"`, followed by one with `type: "summary"`.
Objects carry `schema_version: 1`. Strings preserve valid UTF-8, escape JSON
control characters, and replace malformed UTF-8 with the replacement character.

Sample fields:

- `sample`: report number, starting at 1.
- `uptime_us`: monotonic microseconds since boot at collection completion.
- `requested_interval_us`, `elapsed_us`, `cpu_interval_us`, `collection_us`,
  `lateness_us`: requested interval, actual time between collection completions,
  CPU denominator, collection duration and delay past the collection deadline.
- `skipped_intervals`: deadlines skipped between the preceding report and this one.
- `partial`: separate flags for incomplete task, thread and core collection.
- `cpu`: busy, user, system, scheduler and idle percentages.
- `task_count`, `thread_count`, `zombie_count`, `core_count`, `parked_cores`.
- `memory`: total, usable, used and available bytes; largest free extent;
  signed change in used bytes since the previous observation.
- `clock`: tick and ledger intervals in microseconds, and signed tick-clock
  deviation from the ledger as a percentage.
- With `--cores`, `cores`: core IDs, parked state and CPU breakdowns.
- With `--top`, `top_tasks`: tasks sorted by interval CPU use.
- With `--tid`, `selected_task_id` and `selected_task`: the requested ID and
  task record, including interval event counts and worker-thread records.

With either I/O option, `io_partial` records separate interface, TCP, cache
and clock failures. `--network` adds `network.interfaces` and `network.tcp`;
`--cache` adds `block_cache`. Each source has `available`, `sampled_at_us`,
`interval_us`, and `total`, `delta`, `per_second` counter objects. A counter
object is null when that observation, delta or rate is unavailable. Interface
records also carry `name`, `mac`, and `link_up`; cache records carry `enabled`,
`bytes`, `capacity_bytes`, and interval `hit_percent`. I/O options omitted from
the command produce no corresponding JSON fields and perform no I/O sampling.

Task records use `task_id`; worker records use `thread_id`. Per-task memory
is `heap_bytes`. Counts with `_delta` are events during the sampled interval.
Task/thread `cpu_delta_us` is CPU time consumed during that interval.

Numbers the monitor marks unavailable are `null`, including first CPU
observations, counter resets and missing CPU split fields. An unavailable memory snapshot or selected
task is `null`. A parked core has `parked: true` and null CPU percentages.
`threads` is null when worker details were not sampled (including a task with
one thread); `threads_complete` distinguishes a full worker list from partial
collection. Missing memory on either side makes `used_delta_bytes` null.

Collection is bounded at 512 tasks, 1024 worker threads, 128 cores and
16 network interfaces. Partial
flags accompany truncated or failed optional reads. Files are individual
observations, so a report is not an atomic whole-machine snapshot.

Summary fields include `complete`, `samples`, `cpu_samples`, `memory_samples`,
`partial_samples`, `average_busy_percent`, `peak_busy_percent`,
`min_available_bytes`, `max_interval_us`, `max_lateness_us` and
`skipped_intervals`. A partial sample has incomplete source enumeration or
an unavailable machine CPU or memory measurement, or a failure flagged by
the optional I/O sampler.

Exit status is 0 after the requested reports, 1 on a collection, clock,
allocation or output error, and 2 for invalid arguments. Optional unavailable
fields do not themselves make the command fail. Errors go to stderr. A fatal
error after some reports emits an incomplete summary if output still works;
a baseline failure emits no records.

## Verification

`bash tools/test_sysstat_host.sh` exercises the real command with simulated
samples, clocks and writes, then parses its JSON independently in Python.
`bash tools/test_monitor_host.sh` covers the shared sampling implementation,
including task event deltas across reordered tasks, counter resets and gaps.
`bash tools/test_monitor_io_host.sh` covers the sysfs parser, independent
consumers, interface identity changes, missing sources, resets and rate
arithmetic, including overflow. These suites enable ASan/UBSan; retain leak
detection when running them.

Install the rebuilt `libos64.so` alongside this command: it uses the shared
monitor's task-event accessor and optional I/O API. Host tests do not replace
a live Os64 SSH smoke test.
