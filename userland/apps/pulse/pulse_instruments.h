#ifndef PULSE_INSTRUMENTS_H
#define PULSE_INSTRUMENTS_H
#include "pulse_data.h"
#include "os64/ui.h"

// Pure presentation: readings and peak history are supplied by the owner.
// The renderer performs no I/O, sampling, window operations, or time reads.
typedef struct {
    pulse_sample_t current;
    bool peak_valid[PULSE_DIAL_COUNT];
    uint64_t peak[PULSE_DIAL_COUNT]; // percentages, or bytes/s for network
} pulse_instrument_readings_t;
typedef struct {
    bool compact;
    os64_gui_rect_t bounds, cards[PULSE_METERS], dials[PULSE_DIAL_COUNT], scale;
    bool visible[PULSE_DIAL_COUNT];
    int32_t primary_row, caption_row;
} pulse_instrument_layout_t;
typedef struct pulse_instruments pulse_instruments_t;
// UI is borrowed and must outlive the renderer. Primary readouts use UI;
// captions use DOCUMENT, allowing readable numbers with smaller labels.
pulse_instruments_t *pulse_instruments_create(os64_ui_t *ui);
void pulse_instruments_destroy(pulse_instruments_t *);
// Clear retained text/face caches after font changes. Layout must be recomputed.
void pulse_instruments_invalidate(pulse_instruments_t *);
// Measured native content floor for the selected meters and presentation.
// Normal reserves two columns for multiple cards. Larger rectangles retain
// that arrangement; a narrow/tall renderer layout has a different floor.
// Output is changed only on success; x/y are zero.
bool pulse_instruments_min_size(os64_ui_t *, const pulse_config_t *, bool compact,
                                os64_gui_rect_t *out);
// Layout plans write out only on success. Bounds must lie within a surface
// representable by int32 coordinates. A refusal leaves the prior plan intact.
bool pulse_instruments_layout(os64_ui_t *, const pulse_config_t *, bool compact,
                              os64_gui_rect_t, pulse_instrument_layout_t *out);
// Refusal may leave a partial paint; the owner must replace it with a visible
// error. Drawing is clipped to layout.bounds and the destination surface.
bool pulse_instruments_paint(pulse_instruments_t *, os64_gui_surface_t *,
                            const pulse_instrument_layout_t *,
                            const pulse_config_t *, const pulse_instrument_readings_t *);
int pulse_instruments_hit(const pulse_instrument_layout_t *, int32_t x,int32_t y);
size_t pulse_instruments_live_bytes(const pulse_instruments_t *);
size_t pulse_instruments_peak_bytes(const pulse_instruments_t *);
// Optional face caching; disabling it leaves the same rendering available.
void pulse_instruments_cache(pulse_instruments_t *, bool enabled);
#endif
