// media.c — os64/media.h over libav.so (FFmpeg) and libsound.so.
//
// A media object is a small pipeline, pulled by os64_media_decode:
//
//   source --AVIOContext--> demuxer --packets--> decoder --frames-->
//   media_pcm (to 16-bit stereo at the media's rate) --> resampler (to
//   48 kHz) --> the output buffer --> the caller
//
// THE OUTPUT TIMELINE (os64/media.h) is kept here. The resampler delays
// its output by its latency, so the first output frames of a fresh
// resampler are silence that is not the media's; they are counted off
// (`skip_out`), and at the end of the media the same latency is drawn out
// of it with silence (`tail`), so what comes out is the media's length.
// After a seek the decoder starts at a packet boundary at or before the
// target; the media frames before the target are dropped (`drop_in`)
// before they reach the resampler, so the first frame out is the target.
//
// THREADS. libav is built without threads, so what it shares between
// callers must be set up by one thread at a time (libav/README.md
// § Switched off). In this configuration that is the decoders' static
// tables, made when a decoder opens, and libavutil's CRC tables, which a
// demuxer can first ask for while probing. libmedia makes every CRC table
// at its first open and opens decoders under one lock (`s_lock`), so
// probing, reading and decoding, which may wait on a network, need no
// lock. tools/test_av_audit.py fails when a configuration adds a one-time
// setup elsewhere, which would have to be looked at here.

#include "os64/media.h"

#include "os64/audio.h"
#include "os64/io.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/resample.h"
#include "os64/str.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/channel_layout.h"
#include "libavutil/crc.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"

#include "media_pcm.h"

#define IO_BUFFER 32768

struct os64_media {
    os64_media_source_t src;
    AVIOContext *io;
    AVFormatContext *format;
    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *frame;
    int stream;
    uint32_t rate;
    media_role_t roles[MEDIA_PCM_MAX_CHANNELS];

    os64_resampler_t *resampler;     // NULL when the media is already 48 kHz
    uint32_t skip_out;               // output frames still owed to the resampler's delay
    bool reading_done;               // the demuxer has nothing more
    bool decoding_done;              // the decoder has given its last frame
    bool tail_done;                  // the resampler's held frames have been drawn out
    bool packet_waiting;             // `packet` was refused with EAGAIN and is sent again
    int64_t seek_target;             // media frame wanted after a seek, -1 for none

    int16_t *stereo;                 // one decoded frame, stereo at the media's rate
    uint32_t stereo_cap;
    int16_t *out;                    // stereo at 48 kHz, waiting for the caller
    uint32_t out_cap, out_at, out_len;
    int64_t position;                // output frame of out[out_at]

    char error[160];
};

static os64_lock_t s_lock = OS64_LOCK_INIT;
static bool s_ready;

// ---------------------------------------------------------------- libav's words

// A line of libav's at error level or worse, kept on the media whose
// context said it. Anything else is dropped: a library has no terminal.
static void on_log(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > AV_LOG_ERROR || avcl == NULL) return;
    const AVClass *cls = *(const AVClass *const *)avcl;
    os64_media_t *m = NULL;
    if (cls == avformat_get_class()) m = ((AVFormatContext *)avcl)->opaque;
    else if (cls == avcodec_get_class()) m = ((AVCodecContext *)avcl)->opaque;
    if (m == NULL) return;
    int prefix = 0;
    av_log_format_line2(avcl, level, fmt, vl, m->error, sizeof(m->error), &prefix);
    size_t n = os64_strlen(m->error);
    while (n > 0 && (m->error[n - 1] == '\n' || m->error[n - 1] == '\r')) m->error[--n] = 0;
}

// Once per program, under s_lock: the log callback, and every CRC table, so
// no demuxer makes one while another thread does.
static void ready_locked(void)
{
    if (s_ready) return;
    av_log_set_callback(on_log);
    for (int id = 0; id < AV_CRC_MAX; id++) av_crc_get_table((AVCRCId)id);
    s_ready = true;
}

// ---------------------------------------------------------------- the source

static int io_read(void *opaque, uint8_t *out, int cap)
{
    os64_media_t *m = opaque;
    int64_t n = m->src.read(m->src.ctx, out, (size_t)cap);
    if (n == 0) return AVERROR_EOF;
    if (n < 0) return AVERROR(EIO);
    return (int)n;
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
    os64_media_t *m = opaque;
    if (whence & AVSEEK_SIZE) return m->src.size >= 0 ? m->src.size : AVERROR(ENOSYS);
    whence &= ~AVSEEK_FORCE;
    if (m->src.seek == NULL || whence != SEEK_SET || offset < 0) return AVERROR(ENOSYS);
    int64_t at = m->src.seek(m->src.ctx, (uint64_t)offset);
    return at < 0 ? AVERROR(EIO) : at;
}

// ---------------------------------------------------------------- channels and samples

static media_role_t role_of(enum AVChannel ch)
{
    switch (ch) {
    case AV_CHAN_FRONT_LEFT:   return MEDIA_ROLE_LEFT;
    case AV_CHAN_FRONT_RIGHT:  return MEDIA_ROLE_RIGHT;
    case AV_CHAN_FRONT_CENTER: return MEDIA_ROLE_CENTRE;
    case AV_CHAN_BACK_LEFT:
    case AV_CHAN_SIDE_LEFT:    return MEDIA_ROLE_LEFT_REAR;
    case AV_CHAN_BACK_RIGHT:
    case AV_CHAN_SIDE_RIGHT:   return MEDIA_ROLE_RIGHT_REAR;
    case AV_CHAN_LOW_FREQUENCY:
    case AV_CHAN_LOW_FREQUENCY_2: return MEDIA_ROLE_LFE;
    default:                   return MEDIA_ROLE_OTHER;
    }
}

static bool sample_of(enum AVSampleFormat f, media_sample_t *out, bool *planar)
{
    *planar = av_sample_fmt_is_planar(f);
    switch (av_get_packed_sample_fmt(f)) {
    case AV_SAMPLE_FMT_U8:  *out = MEDIA_SAMPLE_U8;  return true;
    case AV_SAMPLE_FMT_S16: *out = MEDIA_SAMPLE_S16; return true;
    case AV_SAMPLE_FMT_S32: *out = MEDIA_SAMPLE_S32; return true;
    case AV_SAMPLE_FMT_FLT: *out = MEDIA_SAMPLE_F32; return true;
    case AV_SAMPLE_FMT_DBL: *out = MEDIA_SAMPLE_F64; return true;
    default:                return false;   // S64: no decoder libav carries produces it
    }
}

static bool grow(int16_t **buffer, uint32_t *cap, uint32_t frames)
{
    if (frames <= *cap) return true;
    int16_t *bigger = os64_realloc(*buffer, (size_t)frames * 2 * sizeof(int16_t));
    if (bigger == NULL) return false;
    *buffer = bigger;
    *cap = frames;
    return true;
}

// ---------------------------------------------------------------- resampler

static int resampler_start(os64_media_t *m)
{
    os64_resampler_destroy(m->resampler);
    m->resampler = NULL;
    m->skip_out = 0;
    if (m->rate == OS64_AUDIO_RATE) return 0;
    m->resampler = os64_resampler_create(OS64_AUDIO_CHANNELS, m->rate, OS64_AUDIO_RATE);
    if (m->resampler == NULL) return OS64_MEDIA_NO_MEMORY;
    uint64_t latency = os64_resampler_latency(m->resampler);
    m->skip_out = (uint32_t)((latency * OS64_AUDIO_RATE + m->rate / 2) / m->rate);
    return 0;
}

// Media-rate stereo into the output buffer at 48 kHz, the delay counted off.
static int resample_into_out(os64_media_t *m, const int16_t *in, uint32_t frames)
{
    if (m->resampler == NULL) {
        if (!grow(&m->out, &m->out_cap, m->out_len + frames)) return OS64_MEDIA_NO_MEMORY;
        os64_memcpy(m->out + 2 * m->out_len, in, (size_t)frames * 4);
        m->out_len += frames;
        return 0;
    }
    uint32_t done = 0;
    while (done < frames) {
        // Room for the ratio's worth and the resampler's own slack.
        uint32_t room = (uint32_t)(((uint64_t)(frames - done) * OS64_AUDIO_RATE) / m->rate) + 64;
        if (!grow(&m->out, &m->out_cap, m->out_len + room)) return OS64_MEDIA_NO_MEMORY;
        uint32_t take = frames - done, made = room;
        if (os64_resampler_run(m->resampler, in + 2 * done, &take, m->out + 2 * m->out_len, &made) != 0
            || (take == 0 && made == 0))
            return OS64_MEDIA_DECODE;
        done += take;
        uint32_t skip = made < m->skip_out ? made : m->skip_out;
        if (skip > 0) {
            os64_memmove(m->out + 2 * m->out_len, m->out + 2 * (m->out_len + skip),
                         (size_t)(made - skip) * 4);
            m->skip_out -= skip;
        }
        m->out_len += made - skip;
    }
    return 0;
}

// ---------------------------------------------------------------- decoding

// One decoded frame, or the decoder's end. 1: `m->frame` holds sound; 0:
// the decoder has ended; negative: an error.
static int next_frame(os64_media_t *m)
{
    for (;;) {
        int r = avcodec_receive_frame(m->codec, m->frame);
        if (r == 0) return 1;
        if (r == AVERROR_EOF) return 0;
        if (r != AVERROR(EAGAIN)) return OS64_MEDIA_DECODE;

        // The decoder wants input: the packet it refused, or the next.
        if (!m->packet_waiting) {
            if (m->reading_done) return 0;   // a decoder that never reports its end
            r = av_read_frame(m->format, m->packet);
            if (r == AVERROR_EOF) {
                m->reading_done = true;
                r = avcodec_send_packet(m->codec, NULL);
                if (r < 0 && r != AVERROR_EOF) return OS64_MEDIA_DECODE;
                continue;
            }
            if (r < 0) return r == AVERROR(EIO) ? OS64_MEDIA_IO : OS64_MEDIA_DECODE;
            if (m->packet->stream_index != m->stream) {
                av_packet_unref(m->packet);
                continue;
            }
        }
        r = avcodec_send_packet(m->codec, m->packet);
        if (r == AVERROR(EAGAIN)) {
            m->packet_waiting = true;    // receive what it has, then this packet again
            continue;
        }
        m->packet_waiting = false;
        av_packet_unref(m->packet);
        // A damaged packet is skipped, as players do; the decoder says so
        // in the error text. Only a refusal of a different kind ends it.
        if (r < 0 && r != AVERROR_INVALIDDATA) return OS64_MEDIA_DECODE;
    }
}

// The media frame index of the decoded frame's first sample, from its
// timestamp, or -1 when it has none.
static int64_t frame_index(os64_media_t *m)
{
    int64_t pts = m->frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) return -1;
    AVStream *s = m->format->streams[m->stream];
    if (s->start_time != AV_NOPTS_VALUE) pts -= s->start_time;
    return av_rescale_q(pts, s->time_base, (AVRational){ 1, (int)m->rate });
}

// Makes more output, or learns there is none. 1: output was made; 0: the
// media has ended; negative: an error.
static int refill(os64_media_t *m)
{
    m->out_at = m->out_len = 0;
    while (m->out_len == 0) {
        if (!m->decoding_done) {
            int r = next_frame(m);
            if (r < 0) return r;
            if (r == 0) {
                m->decoding_done = true;
                continue;
            }
            const AVFrame *f = m->frame;
            media_pcm_in_t in = { .channels = (uint32_t)f->ch_layout.nb_channels,
                                  .planes = (const uint8_t *const *)f->extended_data,
                                  .frames = (uint32_t)f->nb_samples, .roles = m->roles };
            if (!sample_of(f->format, &in.format, &in.planar)) return OS64_MEDIA_UNSUPPORTED;
            if (!grow(&m->stereo, &m->stereo_cap, in.frames)) return OS64_MEDIA_NO_MEMORY;
            if (!media_pcm_to_stereo(&in, m->stereo)) return OS64_MEDIA_UNSUPPORTED;
            uint32_t first = 0;
            if (m->seek_target >= 0) {
                int64_t at = frame_index(m);
                if (at >= 0 && at < m->seek_target) {
                    int64_t drop = m->seek_target - at;
                    first = drop >= in.frames ? in.frames : (uint32_t)drop;
                }
                if (first < in.frames) m->seek_target = -1;   // the target is in this frame
            }
            av_frame_unref(m->frame);
            int r2 = resample_into_out(m, m->stereo + 2 * first, in.frames - first);
            if (r2 < 0) return r2;
            continue;
        }
        if (m->resampler != NULL && !m->tail_done) {
            // The song is in; what the resampler holds is drawn out with
            // as much silence.
            uint32_t tail = os64_resampler_latency(m->resampler);
            if (!grow(&m->stereo, &m->stereo_cap, tail)) return OS64_MEDIA_NO_MEMORY;
            os64_memset(m->stereo, 0, (size_t)tail * 4);
            m->tail_done = true;
            int r = resample_into_out(m, m->stereo, tail);
            if (r < 0) return r;
            continue;
        }
        return 0;
    }
    return 1;
}

int64_t os64_media_decode(os64_media_t *m, int16_t *out, uint32_t frames)
{
    if (m == NULL || (out == NULL && frames > 0)) return OS64_MEDIA_BAD_ARGS;
    uint32_t given = 0;
    while (given < frames) {
        if (m->out_at == m->out_len) {
            int r = refill(m);
            if (r < 0) return given > 0 ? (int64_t)given : r;
            if (r == 0) break;
        }
        uint32_t n = m->out_len - m->out_at;
        if (n > frames - given) n = frames - given;
        os64_memcpy(out + 2 * given, m->out + 2 * m->out_at, (size_t)n * 4);
        m->out_at += n;
        given += n;
        m->position += n;
    }
    return given;
}

// ---------------------------------------------------------------- open, seek, close

int os64_media_open(const os64_media_source_t *src, os64_media_t **out, os64_media_info_t *info)
{
    if (src == NULL || src->read == NULL || out == NULL) return OS64_MEDIA_BAD_ARGS;
    *out = NULL;
    os64_media_t *m = os64_calloc(1, sizeof(*m));
    if (m == NULL) return OS64_MEDIA_NO_MEMORY;
    m->src = *src;
    m->seek_target = -1;

    os64_lock_acquire(&s_lock);
    ready_locked();
    os64_lock_release(&s_lock);

    int result = OS64_MEDIA_NO_MEMORY;
    uint8_t *buffer = av_malloc(IO_BUFFER);
    if (buffer == NULL) goto fail;
    // io_seek goes in even for a source that cannot seek: it still answers
    // FFmpeg's size question, and an MP3's end padding is trimmed only by a
    // demuxer that knows where the file ends.
    m->io = avio_alloc_context(buffer, IO_BUFFER, 0, m, io_read, NULL, io_seek);
    if (m->io == NULL) { av_free(buffer); goto fail; }
    m->io->seekable = src->seek ? AVIO_SEEKABLE_NORMAL : 0;
    m->format = avformat_alloc_context();
    m->packet = av_packet_alloc();
    m->frame = av_frame_alloc();
    if (m->format == NULL || m->packet == NULL || m->frame == NULL) goto fail;
    m->format->pb = m->io;
    m->format->opaque = m;
    m->format->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&m->format, NULL, NULL, NULL) < 0) {
        result = OS64_MEDIA_NOT_MEDIA;   // and avformat_open_input freed the context
        goto fail;
    }
    if (avformat_find_stream_info(m->format, NULL) < 0) { result = OS64_MEDIA_NOT_MEDIA; goto fail; }
    m->stream = av_find_best_stream(m->format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (m->stream < 0) {
        result = m->stream == AVERROR_DECODER_NOT_FOUND ? OS64_MEDIA_UNSUPPORTED : OS64_MEDIA_NO_SOUND;
        goto fail;
    }
    AVStream *stream = m->format->streams[m->stream];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == NULL) { result = OS64_MEDIA_UNSUPPORTED; goto fail; }
    m->codec = avcodec_alloc_context3(decoder);
    if (m->codec == NULL) goto fail;
    m->codec->opaque = m;
    if (avcodec_parameters_to_context(m->codec, stream->codecpar) < 0) { result = OS64_MEDIA_UNSUPPORTED; goto fail; }
    os64_lock_acquire(&s_lock);
    int opened = avcodec_open2(m->codec, decoder, NULL);
    os64_lock_release(&s_lock);
    if (opened < 0) { result = OS64_MEDIA_UNSUPPORTED; goto fail; }

    int channels = m->codec->ch_layout.nb_channels;
    if (m->codec->sample_rate <= 0 || channels < 1 || channels > MEDIA_PCM_MAX_CHANNELS) {
        result = OS64_MEDIA_UNSUPPORTED;
        goto fail;
    }
    m->rate = (uint32_t)m->codec->sample_rate;
    // A file that does not name its channels (a plain PCM WAV of six) is
    // read as the usual layout for that count, 5.1 for six, as FFmpeg's
    // own resampler reads it.
    AVChannelLayout layout = m->codec->ch_layout;
    if (layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&layout, channels);
    for (int c = 0; c < channels; c++)
        m->roles[c] = role_of(av_channel_layout_channel_from_index(&layout, (unsigned)c));
    if ((result = resampler_start(m)) < 0) goto fail;

    if (info != NULL) {
        os64_memset(info, 0, sizeof(*info));
        os64_strcopy(info->format, sizeof(info->format), m->format->iformat->name);
        os64_strcopy(info->codec, sizeof(info->codec), decoder->name);
        info->rate = m->rate;
        info->channels = (uint32_t)channels;
        info->frames = m->format->duration == AV_NOPTS_VALUE ? -1
                     : av_rescale(m->format->duration, OS64_AUDIO_RATE, AV_TIME_BASE);
        info->seekable = src->seek != NULL;
    }
    *out = m;
    return 0;

fail:
    os64_media_close(m);
    return result;
}

int os64_media_seek(os64_media_t *m, int64_t frame)
{
    if (m == NULL || frame < 0) return OS64_MEDIA_BAD_ARGS;
    if (m->src.seek == NULL) return OS64_MEDIA_CANNOT_SEEK;
    AVStream *s = m->format->streams[m->stream];
    int64_t ts = av_rescale_q(frame, (AVRational){ 1, OS64_AUDIO_RATE }, s->time_base);
    if (s->start_time != AV_NOPTS_VALUE) ts += s->start_time;
    if (av_seek_frame(m->format, m->stream, ts, AVSEEK_FLAG_BACKWARD) < 0) return OS64_MEDIA_CANNOT_SEEK;
    avcodec_flush_buffers(m->codec);
    av_packet_unref(m->packet);
    m->packet_waiting = m->reading_done = m->decoding_done = m->tail_done = false;
    m->out_at = m->out_len = 0;
    m->seek_target = av_rescale(frame, m->rate, OS64_AUDIO_RATE);
    m->position = frame;
    return resampler_start(m);
}

int64_t os64_media_position(const os64_media_t *m)
{
    return m ? m->position : 0;
}

const char *os64_media_error_text(const os64_media_t *m)
{
    return m ? m->error : "";
}

void os64_media_close(os64_media_t *m)
{
    if (m == NULL) return;
    avcodec_free_context(&m->codec);
    avformat_close_input(&m->format);     // NULL already when open_input failed
    if (m->io) {
        av_freep(&m->io->buffer);
        avio_context_free(&m->io);
    }
    av_packet_free(&m->packet);
    av_frame_free(&m->frame);
    os64_resampler_destroy(m->resampler);
    if (m->stereo) os64_free(m->stereo);
    if (m->out) os64_free(m->out);
    os64_free(m);
}

const char *os64_media_strerror(int result)
{
    switch (result) {
    case 0:                      return "no error";
    case OS64_MEDIA_BAD_ARGS:    return "called wrongly";
    case OS64_MEDIA_NOT_MEDIA:   return "not a sound file this can read";
    case OS64_MEDIA_UNSUPPORTED: return "sound in a form this cannot decode";
    case OS64_MEDIA_NO_SOUND:    return "no sound in it";
    case OS64_MEDIA_DECODE:      return "damaged past decoding";
    case OS64_MEDIA_IO:          return "the file could not be read";
    case OS64_MEDIA_NO_MEMORY:   return "not enough memory";
    case OS64_MEDIA_CANNOT_SEEK: return "cannot seek there";
    default:                     return "an error with no name";
    }
}

// ---------------------------------------------------------------- a file source

static int64_t file_read(void *ctx, uint8_t *out, size_t cap)
{
    return os64_read((int32_t)(intptr_t)ctx, out, cap);
}

static int64_t file_seek(void *ctx, uint64_t offset)
{
    return os64_seek((int32_t)(intptr_t)ctx, (int64_t)offset, OS64_SEEK_SET);
}

void os64_media_file_source(int32_t handle, os64_media_source_t *out)
{
    out->ctx = (void *)(intptr_t)handle;
    out->read = file_read;
    out->seek = file_seek;
    int64_t here = os64_seek(handle, 0, OS64_SEEK_CUR);
    int64_t end = here >= 0 ? os64_seek(handle, 0, OS64_SEEK_END) : -1;
    if (here >= 0) os64_seek(handle, here, OS64_SEEK_SET);
    out->size = end;
    if (here < 0) out->seek = NULL;   // not a file with a position: a pipe, the console
}
