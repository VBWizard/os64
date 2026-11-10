#ifndef PULSE_DATA_H
#define PULSE_DATA_H
#include "os64/monitor.h"
#include "os64/monitor_io.h"

enum { PULSE_CPU, PULSE_RAM, PULSE_DISK, PULSE_NET, PULSE_METERS };
enum { PULSE_BYTES, PULSE_KIB, PULSE_MIB, PULSE_BITS, PULSE_KBIT, PULSE_MBIT, PULSE_UNITS };
enum { PULSE_CLASSIC, PULSE_INSTRUMENTS, PULSE_VIEWS };
enum { PULSE_DIAL_CPU, PULSE_DIAL_RAM, PULSE_DIAL_DISK,
       PULSE_DIAL_DOWN, PULSE_DIAL_UP, PULSE_DIAL_COUNT };
enum { PULSE_CLASSIC_SIZE, PULSE_INSTRUMENTS_SIZE, PULSE_COMPACT_SIZE, PULSE_SIZES };
typedef struct {uint32_t w,h;} pulse_size_t;
typedef struct {const char *name;bool compact;} pulse_view_info_t;
extern const pulse_view_info_t pulse_views[PULSE_VIEWS];
typedef struct {
    unsigned view;
    bool instruments_compact;
    int32_t x,y;
    pulse_size_t size[PULSE_SIZES];
    bool show[PULSE_METERS], stay_on_top;
    unsigned units, refresh_ms, font_size;
    char mount[256], interface[64];
} pulse_config_t;
// Measurements carry no widget geometry, colour or display strings. A bar,
// graph or dial can consume the same values without owning a sampler.
typedef struct {
    bool cpu_valid, ram_valid, disk_valid, net_valid;
    uint32_t cpu_percent;
    uint64_t ram_used, ram_available, ram_total;
    uint64_t disk_used, disk_available, disk_total;
    bool peak_valid[PULSE_DIAL_COUNT];
    uint64_t peak[PULSE_DIAL_COUNT]; // percentage or raw bytes/second
    uint64_t network_scale; // shared bar/dial ceiling in bytes/second
    uint64_t download, upload; // bytes/second, regardless of display units
} pulse_sample_t;
// Per-second maxima over a trailing minute, shared by both views. Validity
// bits distinguish a valid zero from a missing sample.
#define PULSE_PEAK_SECONDS 60u
typedef struct {
    uint64_t seconds[PULSE_PEAK_SECONDS],values[PULSE_PEAK_SECONDS][4],last_micros;
    uint8_t valid[PULSE_PEAK_SECONDS];
    bool initialized;
} pulse_history_t;
// Negative/decreasing monotonic time is rejected without changing history;
// the displayed turn has no peak marks and uses a current-rate ceiling.
bool pulse_history_update(pulse_history_t *,int64_t micros,pulse_sample_t *);
void pulse_history_clear_network(pulse_history_t *);
typedef struct {
    pulse_history_t history;
    char history_interface[64];
    os64_monitor_t *system;
    os64_monitor_io_t *network;
} pulse_sampler_t;
unsigned pulse_presentation(const pulse_config_t *);
void pulse_config_defaults(pulse_config_t *);
bool pulse_config_line(const char *, const char *, void *);
bool pulse_sampler_init(pulse_sampler_t *);
void pulse_sampler_destroy(pulse_sampler_t *);
void pulse_sample(pulse_sampler_t *, const pulse_config_t *, pulse_sample_t *);
// Parse the mount report in owned, writable storage. False includes a missing
// mount and filesystems that do not report capacity; neither is zero usage.
bool pulse_mount_space(char *, const char *, uint64_t *, uint64_t *);
bool pulse_network_rate(const os64_monitor_io_snapshot_t *, const char *, uint64_t *, uint64_t *);
unsigned pulse_percent(uint64_t, uint64_t);
void pulse_format_rate(char *, size_t, uint64_t, unsigned);
extern const char *const pulse_unit_names[PULSE_UNITS];
#endif
