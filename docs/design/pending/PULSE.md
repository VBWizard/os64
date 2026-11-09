# Pulse

Pulse is a small GUI system monitor, initially pinned above ordinary windows. Launch `pulse` from a shell.
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
is preferred. Stay on top changes immediately and controls the same native pin
state as the frame button and Ctrl+Alt+P. Frame changes update an open Settings
checkbox; Save as default records the current state for the next launch.
The Settings window follows the gadget into the same stacking band.
Text size accepts 16–48 pixels; larger text grows the minimum
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
reports invalidate it. Network has separate blue download and red upload bars.
They share an automatic ceiling rounded up to a power of two in bytes/second,
with a 1 KiB/s minimum and a trailing 60-second peak. The caption shows this
ceiling in the selected units (or smaller units if it would round to zero); it
measures recent traffic, not link utilization.
A valid nonzero rate gets at least one painted pixel. Missing rates leave empty
tracks. Changing interface resets scale history; changing units does not.
Settings inherits the parent's pinned band so Pulse cannot
cover it merely by occupying a higher window band.

Configuration keys: `cpu`, `ram`, `disk`, `net` (on/off), `network_units`,
`mount`, `interface`, `refresh_ms`, `font_size`, `stay_on_top`. Invalid startup values preserve defaults.

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

The pin control uses `SYSCALL_GUI_WINDOW_SET_PINNED` (66), restricted to owned
ordinary windows. Desktop and popup windows are refused. The frame, keyboard
and syscall paths emit `WINDOW_PIN_CHANGED` (18); clients query current state
rather than replaying stale toggles. Input-loss and periodic readback handle a
dropped hint. This update requires the matching kernel.

`tools/test_window_pin_host.py` exercises the production syscall, ownership
check, frame pin action and WM mutation with host seams, plus event-queue
overflow reconciliation. `/tests/windowpintest` probes the native
ABI with a live desktop. The Pulse UI suite covers checkbox changes, frame
notifications, failed changes and input-loss reconciliation.

The first version passed a hardware tryout: CPU utilization, network rates and
root-partition space were confirmed. The independent text-size revision was
also confirmed readable on hardware.
The network-bar/pin update passed the native pin ABI probe in QEMU; live
checkbox unpinning and keyboard repinning synchronized the Settings state.
On 2026-11-09, hardware acceptance of the revision rebased onto `userland`
`2218cc25` confirmed Stay on top and all four indicators. The reported workload
included compressing files, deleting their originals, a network speed test,
and loading a heavy webpage in Yonder. The matching kernel was installed and
the Stay on top control worked after reboot.

The startup regression after a stock-library refresh was reproduced in QEMU
at RIP `0x6a4033e0`: the installed `libos64.so` lacked Pulse's
`os64_monitor_sample_system` export. Deliver Pulse with a library containing
that entry point. The rebuilt pair passed QEMU startup on the unchanged kernel
and was confirmed working on hardware before the separate pin-kernel update.

Remaining hardware checks include Apply/Save/restart, show/hide/resize,
and closing with the Settings popup open.
Host tests do not establish native window stacking or timer behavior.

## Next disk-activity slice

Use `/sys/disk/stats` for per-device read/write activity. Leave `/sys/mounts`
and `/sys/block` in place; relocating them is a separate migration. Extend the
shared sampler so Pulse and sysstat can consume the same device counters.
Disk activity will be selectable separately from filesystem capacity. Cache
hits are not physical reads; initial device activity should describe whole
physical drives, with partition attribution treated as a separate feature.
