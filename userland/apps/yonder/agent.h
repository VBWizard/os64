#ifndef YONDER_AGENT_H
#define YONDER_AGENT_H

// Who yonder says it is (YONDER.md § Settings): the User-Agent every fetch
// sends, pages, sheets and pictures alike. Sites answer by it, and a person
// comparing yonder with another browser needs to ask as that browser asks.
// The presets are what a person most often wants to be taken for; any
// other is typed.
//
// A PRESET IS A WHOLE IDENTITY: the string, and the lines that browser
// sends beside it on every request (its client hints, its
// Accept-Language), because a site that is told "Chrome" and hears none of
// Chrome's other headers is right not to believe it. A typed agent claims
// nothing beyond itself. What describes the request rather than the
// browser (Fetch Metadata) is libway's, and goes with every agent.

#include <stdbool.h>
#include <stddef.h>

#include "fetch/fetch.h"
#include "way/origin.h"

#define YONDER_AGENT "yonder/1.0 (os64)"
// The longest agent, its NUL included: what a fetch will copy.
#define YONDER_AGENT_MAX OS64_FETCH_AGENT_MAX

typedef struct {
    const char *name;               // what a person picks it by
    const char *agent;
    way_identity_t identity;        // verbatim as that browser sends it
} yonder_agent_preset_t;

size_t yonder_agent_npresets(void);
const yonder_agent_preset_t *yonder_agent_preset(size_t i);

// Whether `agent` can be sent and saved as it is: printable ASCII, no
// space at either end (the config file trims them) and no `#` (it starts a
// comment there), shorter than YONDER_AGENT_MAX. Anything else would be
// refused by the fetch or changed by the file.
bool yonder_agent_valid(const char *agent);

// The preset `agent` is, or NULL for one that was typed.
const char *yonder_agent_name(const char *agent);

// The lines `agent`'s preset sends beside it, or NULL for one that was
// typed. A preset's own, so it lives as long as yonder does.
const way_identity_t *yonder_agent_identity(const char *agent);

#endif
