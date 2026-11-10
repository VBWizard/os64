#ifndef PULSE_PLACEMENT_H
#define PULSE_PLACEMENT_H
#include "pulse_data.h"
#include "os64/gui.h"
// Record completed user placement only. In-memory preferences follow the
// gesture even if persistence fails. Re-read other saved sizes before merging.
int64_t pulse_record_placement(pulse_config_t *,const os64_gui_event_t *);
#endif
