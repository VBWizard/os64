#ifndef JPEG_PORT_CONFIG_H
#define JPEG_PORT_CONFIG_H
#define NO_GETENV
#define NO_PUTENV
#define JPEG_INTERNALS
#include "jinclude.h"
#include "jpeglib.h"
/* Upstream's documented capability switches, applied after its defaults. */
#undef DCT_IFAST_SUPPORTED
#undef DCT_FLOAT_SUPPORTED
#undef D_LOSSLESS_SUPPORTED
#undef C_LOSSLESS_SUPPORTED
#undef QUANT_1PASS_SUPPORTED
#undef QUANT_2PASS_SUPPORTED
#endif
