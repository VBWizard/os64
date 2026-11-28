#ifndef GPLAY_FED_H
#define GPLAY_FED_H
// gplay --fed <prefix>: a media fed in fragments, as a page's MediaSource
// feeds one (os64/media_fed.h), from the files tools/make_dash_fixture.sh
// makes: <prefix>-1080p.mp4, -720p, -480p and -a48, each with its index
// (.json). A FEEDER thread makes every fed call (the API is one thread's):
// it appends whole fragments, keeping four seconds ahead of the position
// main hands it, fetching from wherever the position is after a seek, and
// says the end after the last. `w` switches between the largest and the
// smallest picture rendition from the next fragment; `e` removes what is
// more than a second behind the position.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os64/media_fed.h"

typedef struct gplay_fed gplay_fed_t;

// The renditions loaded, the fed media made, both initialization segments
// and the first fragments appended; NULL with `why` filled on a failure.
gplay_fed_t *gplay_fed_open(const char *prefix, char *why, size_t cap);
os64_media_fed_t *gplay_fed_media(gplay_fed_t *g);
// The feeder started; it runs until gplay_fed_close.
bool gplay_fed_start(gplay_fed_t *g);
// From main: the player's position (output frames), and the keys.
void gplay_fed_position(gplay_fed_t *g, int64_t position);
void gplay_fed_switch(gplay_fed_t *g);
void gplay_fed_evict(gplay_fed_t *g);
// From main: one readout line of what the feeder last saw.
void gplay_fed_line(gplay_fed_t *g, char *line, size_t cap);
// The feeder stopped, the fed media closed (its creator's hold), all freed.
void gplay_fed_close(gplay_fed_t *g);

#endif
