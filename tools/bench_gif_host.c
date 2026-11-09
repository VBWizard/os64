// Decoder/composition CPU time through the public sequence API; no drawing.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>
#include <math.h>
#include "gif_sequence_host_support.h"

static double cpu_time(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) == 0);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void advance(os64_image_sequence_t *seq)
{
    os64_image_status_t status = os64_image_sequence_next(seq);
    if (status == OS64_IMAGE_END)
        status = os64_image_sequence_rewind(seq);
    assert(status == OS64_IMAGE_OK);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    char *end;
    double seconds = strtod(argv[2], &end);
    assert(*end == '\0' && isfinite(seconds) && seconds > 0);
    FILE *in = fopen(argv[1], "rb");
    assert(in && !fseek(in, 0, SEEK_END));
    long length = ftell(in);
    assert(length > 0);
    rewind(in);
    uint8_t *data = malloc((size_t)length);
    assert(data && fread(data, 1, (size_t)length, in) == (size_t)length);
    fclose(in);
    os64_image_sequence_t *seq;
    assert(os64_image_sequence_decode(data, (size_t)length, &seq) == OS64_IMAGE_OK);
    free(data);
    const os64_image_frame_t *frame = os64_image_sequence_frame(seq);
    assert(frame->frame_count > 1);
    // Warm a full pass, then time advances including loop resets. File I/O
    // and opening allocations stay outside the timed interval.
    for (uint32_t i = 0; i < frame->frame_count; i++)
        advance(seq);
    assert(os64_image_sequence_rewind(seq) == OS64_IMAGE_OK);
    size_t opened = attempts, owned = bytes;
    uint64_t frames = 0;
    double start = cpu_time(), elapsed;
    do {
        for (unsigned i = 0; i < 128; i++) {
            advance(seq);
            frames++;
        }
        elapsed = cpu_time() - start;
    } while (elapsed < seconds);
    assert(attempts == opened && bytes == owned);
    printf("%u,%u,%u,%zu,%.6f\n", frame->width, frame->height,
           frame->frame_count, owned, elapsed * 1e6 / frames);
    os64_image_sequence_free(seq);
    assert(!live && !bytes);
    return 0;
}
