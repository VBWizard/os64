/* Keep the window's synthetic animation clock fixtures while exercising real
 * GIF decoding and default-frame acquisition in image ownership tests. */
#define os64_image_sequence_decode fixture_gif_sequence_decode
#define os64_image_sequence_load fixture_gif_sequence_load
#define os64_image_sequence_free fixture_gif_sequence_free
#define os64_image_sequence_frame fixture_gif_sequence_frame
#define os64_image_sequence_default fixture_gif_sequence_default
#define os64_image_sequence_next fixture_gif_sequence_next
#define os64_image_sequence_rewind fixture_gif_sequence_rewind
#include "../userland/libimage/gif.c"
