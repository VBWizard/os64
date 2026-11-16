#ifndef YONDER_PLAYER_H
#define YONDER_PLAYER_H

// player.h — a media element's controls (AUDIO_YONDER.md § The controls):
// one widget laid over the element's box (an audio's whole box, the strip
// along a video's bottom, § Y.4), as a form control's is, drawing a play
// button, the time, a seek bar, a mute button and a volume bar.
//
// It is driven by the element, never the other way round: what it shows is
// the element's view (os64_dom_media_view), handed in by the window, and
// what the person does with it is handed back as what a script would ask
// of the element (os64_dom_media_control_t), so a page hears a click on
// these controls as it hears its own buttons.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dom/media.h"
#include "os64/ui.h"

// The box's size, CSS pixels: Chrome's player's, which pages are laid out
// around.
#define YONDER_PLAYER_WIDTH 300
#define YONDER_PLAYER_HEIGHT 54

typedef struct yonder_player yonder_player_t;

// What the person asked of the element: `value` is the seek's seconds, the
// volume (0 to 1), or mute's on (nonzero).
typedef void (*yonder_player_act_t)(yonder_player_t *p, os64_dom_media_control_t what, double value, void *user);

typedef enum { YONDER_PLAYER_DRAG_NONE, YONDER_PLAYER_DRAG_SEEK, YONDER_PLAYER_DRAG_VOLUME } yonder_player_drag_t;

struct yonder_player {
    os64_ui_widget_t w;             // first: the widget libui knows
    // What it shows (yonder_player_show). A press that asks for a change
    // shows it at once, so a second press before the element's next view
    // asks from there; the next view says what the element did.
    os64_dom_media_view_t view;
    yonder_player_act_t act;
    void *user;
    // A wheel it has no use for (anywhere but the volume), handed on: the
    // page's view scrolls under it, as it does over the page.
    bool (*pass)(const os64_gui_event_t *ev, void *user);
    yonder_player_drag_t drag;      // a press on a bar, until its release
    double drag_value;              // where that bar is held: seconds, or the volume
    uint8_t seq;                    // an arrow key's escape burst, part way
    void *time_run;                 // the time's text, laid out (libui's run slot)
    void *note_run;                 // the line said instead of the bar: busy, or cannot play
};

// Where each part is drawn in `bounds`; a part with no room has width 0.
// As the box narrows the volume bar goes first, then the time, then the
// speaker; the play button stays while the box is a button wide.
typedef struct {
    os64_gui_rect_t play, time, seek, mute, volume;
} yonder_player_parts_t;

extern const os64_ui_class_t yonder_player_class;

void yonder_player(yonder_player_t *p, yonder_player_act_t act,
                   bool (*pass)(const os64_gui_event_t *ev, void *user), void *user);
// The element's view now: repainted when what is drawn changes. True when
// it did.
bool yonder_player_show(os64_ui_t *ui, yonder_player_t *p, const os64_dom_media_view_t *view);
// Its text runs let go, as a page's widget is when the page leaves.
void yonder_player_release(yonder_player_t *p);

// The arithmetic, pure: the parts of a box, "1:12 / 3:45" (with hours when
// the length has them, and "--:--" for a length not known), and the value
// under x on a bar of `length` (seconds or 1 for the volume).
void yonder_player_parts(os64_gui_rect_t bounds, yonder_player_parts_t *out);
size_t yonder_player_time(double position, double duration, char *out, size_t cap);
double yonder_player_value_at(os64_gui_rect_t bar, int32_t x, double length);

#endif
