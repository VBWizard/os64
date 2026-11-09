"""Check the command's emitted JSON using an independent parser."""
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])


def records(name, count, complete=True):
    lines = (root / f"{name}.jsonl").read_text(encoding="utf-8").splitlines()
    parsed = [json.loads(line) for line in lines]
    assert len(parsed) == count + 1, (name, len(parsed))
    assert all(r["schema_version"] == 1 for r in parsed)
    assert [r["sample"] for r in parsed[:-1]] == list(range(1, count + 1))
    assert all(r["type"] == "sample" for r in parsed[:-1])
    assert parsed[-1]["type"] == "summary"
    assert parsed[-1]["samples"] == count
    assert parsed[-1]["complete"] == complete
    return parsed


full = records("full", 3)
for r in full[:-1]:
    assert r["cpu"] == dict(busy_percent=50, user_percent=40, system_percent=10,
                            scheduler_percent=5, idle_percent=45)
    assert r["thread_count"] == 5  # Expanded thread rows exclude single-thread tasks.
    assert [t["task_id"] for t in r["top_tasks"]] == [8, 42]  # No idle/zombie tasks.
    selected = r["selected_task"]
    assert selected["name"] == 'worker"\\\n\x1b café \ufffd'
    assert selected["cpu_percent"] == 150  # Task usage can exceed one core.
    assert selected["switches_delta"] == 17 and selected["major_faults_delta"] == 0
    assert selected["threads_complete"]
    assert [t["thread_id"] for t in selected["threads"]] == [7, 8]
    assert [c["core_id"] for c in r["cores"]] == [0, 7]
    assert r["cores"][1]["parked"] and r["cores"][1]["cpu"]["busy_percent"] is None
    assert r["memory"]["used_delta_bytes"] == 4096
    assert r["clock"]["tick_skew_percent"] == -1.2
assert full[0]["elapsed_us"] == 1002000
assert full[1]["elapsed_us"] == full[2]["elapsed_us"] == 1000000
assert full[-1]["average_busy_percent"] == full[-1]["peak_busy_percent"] == 50

partial = records("partial", 3)
assert partial[0]["memory"] is None and partial[0]["cpu"]["busy_percent"] is None
assert all(partial[0]["partial"].values())
assert partial[1]["memory"]["used_delta_bytes"] is None
assert partial[-1]["partial_samples"] == 1 and partial[-1]["cpu_samples"] == 2

assert all(r["selected_task"] is None for r in records("missing", 3)[:-1])
assert records("single", 3)[0]["selected_task"]["threads"] is None
nosplit = records("nosplit", 3)[0]
assert nosplit["cpu"]["user_percent"] is None and nosplit["cpu"]["busy_percent"] == 50
assert nosplit["selected_task"]["user_percent"] is None

late = records("late", 3)
assert late[0]["lateness_us"] == 250000 and late[0]["elapsed_us"] == 1252000
assert late[1]["lateness_us"] == 0 and late[1]["elapsed_us"] == 750000
assert late[-1]["max_lateness_us"] == 250000

slow = records("slow", 3)
assert slow[0]["collection_us"] == 2500000 and slow[0]["elapsed_us"] == 3500000
assert slow[1]["skipped_intervals"] == 2 and slow[-1]["skipped_intervals"] == 2
assert slow[1]["elapsed_us"] == 502000
slow_output = records("slow_output", 2)
assert slow_output[1]["skipped_intervals"] == 1 and slow_output[1]["elapsed_us"] == 2000000

one = records("one", 1)
assert one[0]["requested_interval_us"] == 250000 and one[0]["elapsed_us"] == 252000
assert "selected_task" not in one[0] and "cores" not in one[0] and "top_tasks" not in one[0]
unavailable = records("unavailable", 1)[-1]
assert unavailable["average_busy_percent"] is None and unavailable["min_available_bytes"] is None
assert unavailable["cpu_samples"] == unavailable["memory_samples"] == 0
zero = records("zero", 1)
assert zero[0]["cpu"]["busy_percent"] == zero[-1]["average_busy_percent"] == 0
assert len(records("large", 1)[0]["cores"]) == 128
records("failed", 1, complete=False)
print("sysstat: JSON parsing, rates, missing values, identities, escaping and cadence PASS")

assert "network" not in one[0] and "block_cache" not in one[0]
io = records("io", 2)
for row in io[:-1]:
    assert not any(row["io_partial"].values())
    nic = row["network"]["interfaces"][0]
    assert nic["name"] == "eth0" and nic["mac"] == "00:11:22:33:44:55" and nic["link_up"]
    assert nic["total"]["rx_bytes"] == 5000 and nic["delta"]["rx_bytes"] == 1000
    assert nic["per_second"]["rx_bytes"] == 1000 and nic["interval_us"] == 1000000
    assert row["network"]["tcp"]["per_second"]["retransmits"] == 2
    cache = row["block_cache"]
    assert cache["enabled"] and cache["bytes"] == 4096 and cache["capacity_bytes"] == 8192
    assert cache["hit_percent"] == 80 and cache["delta"]["hits"] == 8
unavailable_io = records("io_unavailable", 2)
for row in unavailable_io[:-1]:
    nic = row["network"]["interfaces"][0]
    assert not nic["available"] and nic["mac"] is None and nic["link_up"] is None
    assert nic["total"] is None and nic["delta"] is None and nic["per_second"] is None
    assert row["network"]["tcp"]["total"] is None and row["block_cache"]["bytes"] is None
    assert row["block_cache"]["hit_percent"] is None
assert unavailable_io[-1]["partial_samples"] == 2
print("sysstat: optional network/cache JSON and unavailable data PASS")
