#ifndef PULSE_DATA_H
#define PULSE_DATA_H
#include "os64/monitor.h"
#include "os64/monitor_io.h"

enum { PULSE_CPU, PULSE_RAM, PULSE_DISK, PULSE_NET, PULSE_METERS };
enum { PULSE_BYTES, PULSE_KIB, PULSE_MIB, PULSE_BITS, PULSE_KBIT, PULSE_MBIT, PULSE_UNITS };
typedef struct {
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
    uint64_t network_scale; // shared bar ceiling in bytes/second
    uint64_t download, upload; // bytes/second, regardless of display units
} pulse_sample_t;
// One peak per elapsed second bounds history independently of refresh rate.
typedef struct {
    uint64_t seconds[60], peaks[60], last_second;
    bool initialized;
} pulse_rate_scale_t;
uint64_t pulse_rate_scale(pulse_rate_scale_t *,uint64_t second,uint64_t down,uint64_t up,bool valid);
typedef struct {
    pulse_rate_scale_t scale;
    char scale_interface[64];
    os64_monitor_t *system;
    os64_monitor_io_t *network;
} pulse_sampler_t;
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
