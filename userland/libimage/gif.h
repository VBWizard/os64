#ifndef OS64_IMAGE_GIF_H
#define OS64_IMAGE_GIF_H
#include "image/image.h"

// Private libimage entry point; the dispatcher has verified the signature
// and cleared out. Failure leaves out empty.
os64_image_status_t image_decode_gif(const uint8_t *data, size_t len,
                                      os64_image_t *out);
#endif
