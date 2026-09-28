#ifndef TEST_LIBFLOW_FONTS_H
#define TEST_LIBFLOW_FONTS_H

#include "os64/font_backend.h"

// The backend libflow's layout dumps are computed against by hand; the
// metrics are listed at the top of test_libflow_fonts.c.
const os64_font_backend_t *flow_test_backend(void);

#endif
