#ifndef OS64_MONITOR_IO_H
#define OS64_MONITOR_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os64_monitor_io os64_monitor_io_t;

enum {
    OS64_MONITOR_IO_NETWORK = 1u << 0,
    OS64_MONITOR_IO_CACHE = 1u << 1
};
enum {
    OS64_MONITOR_IO_INTERFACES_PARTIAL = 1u << 0,
    OS64_MONITOR_IO_TCP_UNAVAILABLE = 1u << 1,
    OS64_MONITOR_IO_CACHE_UNAVAILABLE = 1u << 2,
    OS64_MONITOR_IO_CLOCK_UNAVAILABLE = 1u << 3
};

typedef struct {
    bool available, clock_valid, delta_valid, rate_valid;
    uint64_t sampled_at_us, interval_us;
} os64_monitor_io_status_t;

typedef struct {
    uint64_t rx_bytes, tx_bytes, rx_frames, tx_frames, rx_errors, tx_errors;
    uint64_t rx_dropped_no_handler, rx_dropped_too_big;
} os64_monitor_net_values_t;

typedef struct {
    char name[64], mac[18];
    bool link_up;
    os64_monitor_io_status_t status;
    os64_monitor_net_values_t total, delta, per_second;
} os64_monitor_interface_t;

typedef struct {
    uint64_t segments_in, segments_out, retransmits, fast_retransmits;
    uint64_t tx_local_drops, bad_checksum, connect_timeouts, resets_received;
} os64_monitor_tcp_values_t;

typedef struct {
    os64_monitor_io_status_t status;
    os64_monitor_tcp_values_t total, delta, per_second;
} os64_monitor_tcp_t;

typedef struct {
    uint64_t hits, misses, fills, evictions, updates, bypass_edge, discarded_races;
} os64_monitor_cache_values_t;

typedef struct {
    bool enabled, hit_percent_valid;
    uint32_t hit_percent; // interval hits / (hits + misses), truncated; no traffic is unavailable
    uint64_t bytes, capacity;
    os64_monitor_io_status_t status;
    os64_monitor_cache_values_t total, delta, per_second;
} os64_monitor_cache_t;

typedef struct {
    uint32_t sources, partial;
    const os64_monitor_interface_t *interfaces;
    size_t interface_count;
    os64_monitor_tcp_t tcp;
    os64_monitor_cache_t cache;
} os64_monitor_io_snapshot_t;

// Optional companion to the CPU/task monitor. Each instance owns its history;
// callers serialize access to an instance. No terminal, sleep or display code.
// Sources must be a nonempty combination of the flags above. Network sampling
// requires a positive interface capacity. Allocation failure returns NULL.
os64_monitor_io_t *os64_monitor_io_create(uint32_t sources, size_t interfaces);
void os64_monitor_io_destroy(os64_monitor_io_t *monitor);

// Sample /sys/net NIC reports and TCP counters, and/or /sys/cache. Returns 0
// with a borrowed snapshot valid until the next call or destruction; -1 for
// invalid arguments. Optional read failures set partial flags and invalidate
// that source's history. Retained interface records can be unavailable.
// Interfaces match by name and MAC, independently of enumeration order.
//
// Totals and per-second rates retain the producer's units: network bytes and
// frames, TCP segments/events, cache request segments (hits/misses/bypass_edge) and cache
// line events. Cache fills are not whole-device I/O counts or byte throughput.
// Each source uses its own monotonic observation interval. Rates round down;
// missing baselines, counter resets, failed clocks, zero intervals and rate
// overflow have explicit validity flags. No-traffic intervals have valid zero
// rates. Adding sources does not change the CPU monitor's public layouts.
int32_t os64_monitor_io_sample(os64_monitor_io_t *monitor,
                               const os64_monitor_io_snapshot_t **out);

#endif
