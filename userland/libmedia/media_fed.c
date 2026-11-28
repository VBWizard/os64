// libmedia's fed media (os64/media_fed.h). Every call refuses until the fed
// path is built: MSE_TRACKER.csv's MS.2 tracks it.
#include "os64/media_fed.h"
#include "os64/media.h"

int os64_media_fed_create(os64_media_fed_t **out)
{
    if (out != 0) *out = 0;
    return OS64_MEDIA_UNSUPPORTED;
}
void os64_media_fed_close(os64_media_fed_t *fed) { (void)fed; }
bool os64_media_fed_type_supported(const char *type) { (void)type; return false; }
os64_media_fed_buffer_t *os64_media_fed_add(os64_media_fed_t *fed, const char *type) { (void)fed; (void)type; return 0; }
void os64_media_fed_remove_buffer(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf) { (void)fed; (void)buf; }
bool os64_media_fed_evict(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, size_t incoming, double play)
{
    (void)fed; (void)buf; (void)incoming; (void)play;
    return false;
}
bool os64_media_fed_take(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, const uint8_t *bytes, size_t length)
{
    (void)fed; (void)buf; (void)bytes; (void)length;
    return false;
}
static void refused(os64_media_fed_result_t *out)
{
    if (out == 0) return;
    *out = (os64_media_fed_result_t){ .status = OS64_MEDIA_FED_ERROR, .init_duration = __builtin_nan(""),
                                      .group_end = __builtin_nan(""), .highest_pts = __builtin_nan("") };
    const char *why = "the fed media is not built";
    for (int i = 0; why[i] && i < (int)sizeof(out->message) - 1; i++) out->message[i] = why[i];
}
void os64_media_fed_parse(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_result_t *out)
{
    (void)fed; (void)buf; refused(out);
}
void os64_media_fed_poll(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_result_t *out)
{
    (void)fed; (void)buf; refused(out);
}
void os64_media_fed_reset(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_result_t *out)
{
    (void)fed; (void)buf; refused(out);
}
void os64_media_fed_remove(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, double start, double end,
                           os64_media_fed_result_t *out)
{
    (void)fed; (void)buf; (void)start; (void)end; refused(out);
}
void os64_media_fed_set(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_mode_t mode,
                        double timestamp_offset, os64_media_fed_group_t group, double window_start,
                        double window_end)
{
    (void)fed; (void)buf; (void)mode; (void)timestamp_offset; (void)group; (void)window_start; (void)window_end;
}
uint32_t os64_media_fed_ranges(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, bool ended,
                               os64_media_fed_range_t *out, uint32_t cap, double *highest_end)
{
    (void)fed; (void)buf; (void)ended; (void)out; (void)cap;
    if (highest_end != 0) *highest_end = __builtin_nan("");
    return 0;
}
void os64_media_fed_duration(os64_media_fed_t *fed, double seconds) { (void)fed; (void)seconds; }
void os64_media_fed_end(os64_media_fed_t *fed) { (void)fed; }
void os64_media_fed_counts(os64_media_fed_t *fed, os64_media_fed_counts_t *out)
{
    (void)fed;
    if (out != 0) *out = (os64_media_fed_counts_t){ 0 };
}
