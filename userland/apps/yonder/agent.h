#ifndef YONDER_AGENT_H
#define YONDER_AGENT_H

// Who yonder says it is (YONDER.md § Settings): the User-Agent every fetch
// sends, pages, sheets and pictures alike. Sites answer by it, and a person
// comparing yonder with another browser needs to ask as that browser asks.
// The presets are what a person most often wants to be taken for; any
// other is typed.

#include <stdbool.h>
#include <stddef.h>

#include "fetch/fetch.h"

#define YONDER_AGENT "yonder/1.0 (os64)"
// The longest agent, its NUL included: what a fetch will copy.
#define YONDER_AGENT_MAX OS64_FETCH_AGENT_MAX

typedef struct {
    const char *name;               // what a person picks it by
    const char *agent;
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

#endif
