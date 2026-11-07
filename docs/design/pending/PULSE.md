# Pulse

Pulse is a small, pinned GUI system monitor. Launch `pulse` from a shell.
It opens without taking keyboard focus, remembers position and size, and
uses its own 24-pixel DejaVu Sans meter text. The first layout is a vertical
panel with text and usage bars. The panel follows toolkit colours, independently
of the desktop Interface font; the Settings dialog uses the normal Interface font.

## Controls

Open Settings with the panel button or the titlebar Settings action. Each
meter can be shown independently; hiding all four leaves Settings accessible.
Apply affects this window. Save as default also updates `pulse.conf` through
the shared configuration writer, preserving unrelated keys and saved geometry.
Resize the panel after hiding meters or reducing text size if a smaller window
is preferred. Text size accepts 16–48 pixels; larger text grows the minimum
window envelope to fit. A font-load or native-size failure refuses Apply and
keeps the previous settings and font. The installed face is
`/etc/fonts/DejaVuSans.ttf`.

| Meter | Display | Default |
| --- | --- | --- |
| CPU | Overall utilization, including scheduler time, normalized across cores | Shown |
| RAM | Physical memory used and available, with a used/usable bar | Shown |
| Disk | Used and free space for one exact mount, with a used/total bar | Shown, `/` |
| Network | Receive and transmit bytes per second converted to selected units | Shown, all non-loopback interfaces |

Network units are B/s, KiB/s, MiB/s, bit/s, kbit/s and Mbit/s. Byte multiples
are binary; bit multiples are decimal. MiB/s is the default. The refresh
interval defaults to 1000 ms and accepts 250–10000 ms. A specific interface
name can replace `all`; `all` excludes `lo`. Disk selection is a mount point,
not an arbitrary path or a physical drive's I/O rate.

Unavailable data is labelled rather than represented as zero. CPU and network
need a complete interval before displaying a rate; resets and incomplete
reports invalidate it. Network has no percentage bar because link capacity is
not measured. Settings inherits the parent's pinned band so Pulse cannot
cover it merely by occupying a higher window band.

Configuration keys: `cpu`, `ram`, `disk`, `net` (on/off), `network_units`,
`mount`, `interface`, `refresh_ms`, `font_size`. Invalid startup values preserve defaults.

## Structure

`pulse_data.c` owns configuration, samplers, report parsing, checked unit
conversion and raw measurements. `pulse.c` owns widgets, rendering and window
lifetimes. A future renderer can use these measurements for graphs or dials
without reading kernel reports itself. There is no skin loader or history
buffer in this version; per-core indicators and skins remain follow-ons.

CPU and RAM use the shared monitor's system-only sample entry point. It reads
core ledgers and physical memory without enumerating processes or threads.
Existing full monitor sampling retains its behavior. Switching from system
sampling back to full sampling rebaselines tasks and threads. Network uses
`monitor_io`; disk capacity uses the same `/sys/mounts` format as `df`.

A timer thread rings the main window. The dispatcher thread samples and
changes widgets, including the Settings window. Shutdown joins the timer
before destroying the window it rings. CPU/network baselines continue while
meters are hidden. The selected disk is sampled only while its meter is shown.

## Validation and remaining acceptance

- `tools/test_pulse_host.sh`: mount parsing, unsupported/missing capacity,
  aggregation and unavailable network intervals, overflow-safe percentages and
  units, configuration and sample validity (ASan, UBSan, LeakSanitizer).
- `tools/test_monitor_host.sh`: existing consumers plus system-only sampling,
  `/proc` independence and switching back to full sampling.
- `python3 tools/test_pulse_ui_host.py`: production widgets with real fonts,
  all visibility combinations, Apply/Save validation and save failure,
  dynamic captions, independent 24–48 pixel text, geometry growth, font-load
  and resize refusal, appearance-event independence and Settings creation flags.
  Native window I/O is stubbed. `--build-only` supports running the resulting
  binary outside a sandbox while retaining leak detection. The runner writes
  a host-rendered `pulse.ppm` preview.
- Native `pulse` target built with the project's warning-as-error toolchain.

The first version passed a hardware tryout: CPU utilization, network rates and
root-partition space were confirmed. The independent text-size revision still
needs hardware acceptance. Remaining checks include RAM, Apply/Save/restart,
show/hide/resize, topmost behavior and closing with the Settings popup open.
Host tests do not establish native window stacking or timer behavior.
