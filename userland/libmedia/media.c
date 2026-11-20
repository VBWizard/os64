// media.c — os64/media.h over libav.so (FFmpeg) and libsound.so.
//
// A media object is a small pipeline, pulled by os64_media_decode or
// os64_media_next:
//
//   source --AVIOContext--> demuxer --packets--> sound decoder --frames-->
//   media_pcm (to 16-bit stereo at the media's rate) --> resampler (to
//   48 kHz) --> the output buffer --> the caller
//                                 \--packets--> picture decoder --frames-->
//                                   a picture handle --> the caller
//
// One demuxer serves both decoders, because an MP4 interleaves them: each
// packet read goes onto the list of its stream's kind, and each decoder
// takes its input from its own list (THE PACKET LISTS), so a caller taking
// one kind for a while is not starved of it by the other. A stream that
// was not wanted is discarded at the demuxer, so no packet of it is made.
//
// THE OUTPUT TIMELINE (os64/media.h) is kept here. Its zero is the FILE'S
// start: the earliest start of the sound and picture streams (`zero_ts`,
// in the time base of the stream that has it), found the same way for
// every open of a file. Each stream's LEAD is how far after that zero it
// starts, in output frames; the stream that starts first has none. The
// sound's lead comes out first as silence, so the sound is contiguous from
// frame 0; a picture's time is its own offset from its stream's start plus
// its stream's lead.
//
// The resampler delays its output by its latency, so the first output
// frames of a fresh resampler are silence that is not the media's; they
// are counted off (`skip_out`), and at the end of the media the same
// latency is drawn out of it with silence (`tail`), so what comes out is
// the media's length. After a seek the decoder starts a little before the
// target, so the fresh resampler has the song to work from (os64_media_seek
// says how much); media frames before that start (`seek_target`) are
// dropped, and the output before the target is counted off with the delay,
// so the first frame out is the target, as an unbroken decode would give
// it. Pictures before a seek's target are dropped too, but for the last of
// them (`seek_held`), which is the one showing AT the target.
//
// THREADS. libav is built without threads, so what it shares between
// callers must be set up by one thread at a time (libav/README.md
// § Switched off). In this configuration that is libavutil's CRC tables,
// which a demuxer can first ask for while reading a header, its AES
// tables, which the MP4 reader asks for while reading a packet of a file
// with an `adrm` atom, the decoders' static tables, made when a decoder
// opens: by this media's own opens, and by avformat_find_stream_info,
// which opens decoders of its own to learn the sample format, and the
// scaler's rgb2rgb tables, made when a scaler first builds its graph,
// which media_scale.c's scalers do on whatever thread converts. (AAC's SBR
// asks for transform tables again while decoding, but only ones its
// decoder's open already made.) So libmedia makes every CRC table, the AES
// tables and the scaler's tables at its first open, and takes one lock
// (`s_lock`) across the whole of an open (the input, its header,
// find_stream_info and the decoders' opens) and the whole of a close:
// libav's rule for a build without threads (README.md), which libmedia
// had once narrowed to find_stream_info and the decoders' opens. Two
// inputs opened at once were seen to hang (a page's two videos, 2 of 4
// QEMU boots); what they share is not named (DEBTS), so the lock stays
// wide. Reading packets, decoding and converting take no lock.
//
// WHAT IS HELD ACROSS A BYTE-SOURCE READ: an open reads its source (a
// page's comes across the network) under `s_lock`, so `s_lock` is taken by
// os64_media_open_for and os64_media_close alone, never by a release,
// os64_media_next, the scaler or anything a player's audio or main thread
// calls; tools/test_av_audit.py holds that. The cost: two media opening at
// once take turns, one waiting out the other's header and
// find_stream_info, which reads a few packets of a sound and, for an open
// that wants the picture, decodes its first pictures too (up to some
// twenty of an MP4's), from the source; and a close waits for an open in
// flight, on the thread that closes (a player's decoding thread).
// A sound-only open discards the picture's stream before it, so none of
// its packets is made or decoded. tools/test_av_audit.py pins every one-time setup in
// the build, so a configuration that adds one fails until it has been
// looked at here.
//
// A PICTURE'S MEMORY belongs to its decoder's frame pool, and FFmpeg's
// pools and reference counts are shared with the decoder without a lock
// of their own in this build (AVBufferPool's mutex is an empty function
// without threads). So a picture is RELEASED from any thread onto the
// media's `released` list, under `release_lock`, and the list is freed
// with av_frame_free on the decoding thread, at the top of every call that
// touches the decoder (`drain_released`). Every call that can touch an
// FFmpeg reference count or pool therefore runs on the decoding thread,
// and the converting thread only reads a picture's planes
// (media_scale.c). tools/test_av_audit.py holds both halves: no call into
// libav.so is reachable from os64_media_picture_release, and from
// os64_media_scaler_convert only the scaler's own.

#include "os64/media.h"

#include "os64/audio.h"
#include "os64/io.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/resample.h"
#include "os64/str.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/aes.h"
#include "libavutil/channel_layout.h"
#include "libavutil/crc.h"
#include "libavutil/display.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libswscale/swscale.h"

#include "media_pcm.h"
#include "media_picture.h"

#define IO_BUFFER 32768
#define NEED_INPUT 1000                  // a step's answer: its decoder wants a packet

static const AVRational kOut = { 1, OS64_AUDIO_RATE };

enum { KIND_SOUND = 0, KIND_PICTURE = 1 };   // the packet lists' indices

typedef struct listed {
    AVPacket *packet;
    struct listed *next;
} listed_t;

typedef struct {
    listed_t *head, *tail;
    uint32_t packets;
    uint64_t bytes;
} packet_list_t;

struct os64_media {
    os64_media_source_t src;
    uint32_t want;                   // OS64_MEDIA_WANT_*
    AVIOContext *io;
    AVFormatContext *format;
    AVPacket *packet;                // the demuxer's, for one read
    bool reading_done;               // the demuxer has nothing more
    packet_list_t list[2];           // THE PACKET LISTS, by KIND_*
    bool flushed[2];                 // that decoder has been told the input ended

    int64_t zero_ts;                 // the file's start, in zero_tb
    AVRational zero_tb;
    int found[2];                    // the file's sound and picture streams, decoded or not; -1 for none

    // The sound: active when `codec` is open.
    AVCodecContext *codec;
    AVFrame *frame;
    int stream;                      // -1 for none
    int64_t sound_lead;              // output frames from the zero to the sound's start
    uint32_t rate;
    media_role_t roles[MEDIA_PCM_MAX_CHANNELS];

    os64_resampler_t *resampler;     // NULL when the media is already 48 kHz
    uint32_t skip_out;               // output frames still to count off: the delay, and a seek's run-in
    int64_t silence;                 // output frames of silence still owed before the sound (its lead)
    bool decoding_done;              // the decoder has given its last frame
    bool tail_done;                  // the resampler's held frames have been drawn out
    int64_t seek_target;             // media frame wanted after a seek, -1 for none

    int16_t *stereo;                 // one decoded frame, stereo at the media's rate
    uint32_t stereo_cap;
    int16_t *out;                    // stereo at 48 kHz, waiting for the caller
    uint32_t out_cap, out_at, out_len;
    int64_t position;                // output frame of out[out_at]

    // The picture: active when `pcodec` is open.
    AVCodecContext *pcodec;
    AVFrame *pframe;
    int pstream;                     // -1 for none
    int64_t picture_lead;            // output frames from the zero to the picture's start
    AVRational picture_rate;         // as the file states it; 0/0 when not
    bool picture_done;               // the picture decoder has given its last frame
    bool resync;                     // dropping picture packets until a keyframe
    int64_t seek_picture;            // output frame a seek wants the picture for, -1 for none
    media_picture_t *seek_held;      // the last picture at or before it, so far
    media_picture_t *ready[2];       // made, not yet given: ready[0] goes first
    os64_lock_t release_lock;        // guards `released`, which any thread may push
    media_picture_t *released;
    uint64_t pictures_given, pictures_returned;

    os64_media_counts_t counts;
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

// Once per program, under s_lock: the log callback, every CRC table, the
// AES tables, and the scaler's rgb2rgb tables, so no demuxer makes one
// while another thread does (the MP4 reader runs AES over each packet of a
// file with an `adrm` atom, inside av_read_frame, which takes no lock) and
// no two scalers make the scaler's at their first conversions at once.
// The scaler makes its tables in sws_init_context, which one small context
// initialised here reaches.
static void ready_locked(void)
{
    if (s_ready) return;
    av_log_set_callback(on_log);
    for (int id = 0; id < AV_CRC_MAX; id++) av_crc_get_table((AVCRCId)id);
    struct AVAES *aes = av_aes_alloc();
    if (aes != NULL) {
        static const uint8_t kZeroKey[16];
        (void)av_aes_init(aes, kZeroKey, 128, 0);
        av_free(aes);
    }
    SwsContext *sws = sws_getContext(16, 16, AV_PIX_FMT_YUV420P, 16, 16, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL,
                                     NULL);
    sws_freeContext(sws);
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

// What the resampler holds is drawn out with as much silence, into the
// output. Nothing for media at 48 kHz.
static int resampler_drain(os64_media_t *m)
{
    if (m->resampler == NULL) return 0;
    uint32_t tail = os64_resampler_latency(m->resampler);
    if (!grow(&m->stereo, &m->stereo_cap, tail)) return OS64_MEDIA_NO_MEMORY;
    os64_memset(m->stereo, 0, (size_t)tail * 4);
    return resample_into_out(m, m->stereo, tail);
}

// ---------------------------------------------------------------- the timeline

static int64_t start_of(const AVStream *s)
{
    return s->start_time == AV_NOPTS_VALUE ? 0 : s->start_time;
}

// Output frames from the file's zero to `s`'s start.
static int64_t lead_of(const os64_media_t *m, const AVStream *s)
{
    return av_rescale_q(start_of(s), s->time_base, kOut) - av_rescale_q(m->zero_ts, m->zero_tb, kOut);
}

// The zero: the earlier start of the sound stream and the picture stream
// (either may be -1), in the time base of the stream that has it, so the
// stream that starts first has a lead of exactly 0.
static void find_zero(os64_media_t *m, int sound, int picture)
{
    m->zero_ts = 0;
    m->zero_tb = (AVRational){ 1, 1 };
    bool have = false;
    int streams[2] = { sound, picture };
    for (int i = 0; i < 2; i++) {
        if (streams[i] < 0) continue;
        const AVStream *s = m->format->streams[streams[i]];
        if (!have || av_compare_ts(start_of(s), s->time_base, m->zero_ts, m->zero_tb) < 0) {
            m->zero_ts = start_of(s);
            m->zero_tb = s->time_base;
            have = true;
        }
    }
}

// ---------------------------------------------------------------- the demuxer

// THE PACKET LISTS. Every packet read is put on the list of its kind, and
// each decoder takes its input from its own list, in the file's order. So
// a caller that takes only one kind for a while (os64_media_next's
// `accept`: its picture queue is full, say) is not starved of it by the
// other: the demuxer reads on past the other kind's packets, which wait on
// their list, undecoded, until that kind is taken again. Once a list's
// packet data reaches LIST_BYTES, the kind not being taken must be taken
// before the demuxer reads on (OS64_MEDIA_GOT_FULL), so a list passes the
// bound by one packet at most; the bookkeeping beside each packet is not
// counted. Eight MiB is some five seconds of a 1440p60 H.264 picture and
// minutes of any sound. A packet a decoder refuses
// (EAGAIN) goes back to the front of its list, to be sent again after the
// decoder's output has been received. At the demuxer's end each decoder is
// told the input has ended once its list is empty.
#define LIST_BYTES (8u << 20)

static int list_push(packet_list_t *l, AVPacket *owned, bool front)
{
    listed_t *e = os64_malloc(sizeof(*e));
    if (e == NULL) return OS64_MEDIA_NO_MEMORY;
    e->packet = owned;
    e->next = NULL;
    if (front) {
        e->next = l->head;
        l->head = e;
        if (l->tail == NULL) l->tail = e;
    } else {
        if (l->tail != NULL) l->tail->next = e;
        else l->head = e;
        l->tail = e;
    }
    l->packets++;
    l->bytes += (uint64_t)owned->size;
    return 0;
}

// The front packet, now the caller's (av_packet_free), or NULL.
static AVPacket *list_pop(packet_list_t *l)
{
    listed_t *e = l->head;
    if (e == NULL) return NULL;
    l->head = e->next;
    if (l->head == NULL) l->tail = NULL;
    l->packets--;
    l->bytes -= (uint64_t)e->packet->size;
    AVPacket *p = e->packet;
    os64_free(e);
    return p;
}

static void list_clear(packet_list_t *l)
{
    AVPacket *p;
    while ((p = list_pop(l)) != NULL) av_packet_free(&p);
}

// One packet from the demuxer onto the list of its kind: 0 when one was
// listed, or dropped (a stream nothing decodes, a picture before a
// resync's keyframe), or the demuxer's end was reached; negative on an
// error.
static int read_packet(os64_media_t *m)
{
    int r = av_read_frame(m->format, m->packet);
    if (r == AVERROR_EOF) {
        m->reading_done = true;
        return 0;
    }
    if (r < 0) return r == AVERROR(EIO) ? OS64_MEDIA_IO : OS64_MEDIA_DECODE;
    int kind;
    if (m->codec != NULL && m->packet->stream_index == m->stream) {
        m->counts.sound_packets++;
        kind = KIND_SOUND;
    } else if (m->pcodec != NULL && m->packet->stream_index == m->pstream) {
        m->counts.picture_packets++;
        kind = KIND_PICTURE;
        if (m->resync) {
            if (!(m->packet->flags & AV_PKT_FLAG_KEY)) {
                m->counts.pictures_resync++;
                av_packet_unref(m->packet);
                return 0;
            }
            // The keyframe: decoding starts again from it (the decoder was
            // flushed when the resync was asked for).
            m->resync = false;
        }
    } else {
        av_packet_unref(m->packet);
        return 0;
    }
    AVPacket *owned = av_packet_alloc();
    if (owned == NULL) {
        av_packet_unref(m->packet);
        return OS64_MEDIA_NO_MEMORY;
    }
    av_packet_move_ref(owned, m->packet);
    if ((r = list_push(&m->list[kind], owned, false)) < 0) av_packet_free(&owned);
    return r;
}

// Input for the decoder of `kind`, which wants it: its next listed packet,
// or, its list empty at the demuxer's end, the news that the input has
// ended. 0 when the decoder was given something, or has already been given
// everything (its step then sees the end); NEED_INPUT when its list is
// empty and the demuxer must be read; negative on an error.
static int feed(os64_media_t *m, int kind)
{
    AVCodecContext *c = kind == KIND_SOUND ? m->codec : m->pcodec;
    AVPacket *p = list_pop(&m->list[kind]);
    if (p == NULL) {
        if (!m->reading_done) return NEED_INPUT;
        if (m->flushed[kind]) return 0;     // nothing more to give; the step sees the end
        m->flushed[kind] = true;
        int r = avcodec_send_packet(c, NULL);
        return r < 0 && r != AVERROR_EOF ? OS64_MEDIA_DECODE : 0;
    }
    int r = avcodec_send_packet(c, p);
    if (r == AVERROR(EAGAIN)) {
        // Sent again after the decoder's output has been received.
        if ((r = list_push(&m->list[kind], p, true)) < 0) av_packet_free(&p);
        return r;
    }
    av_packet_free(&p);
    // Damaged media is skipped, as players do (the decoder says so in the
    // error text); only a refusal of a different kind ends the decode.
    if (r < 0 && r != AVERROR_INVALIDDATA) return OS64_MEDIA_DECODE;
    return 0;
}

// Whether the decoder of `kind` has been given everything it ever will.
static bool input_ended(const os64_media_t *m, int kind)
{
    return m->flushed[kind] && m->list[kind].head == NULL;
}

// ---------------------------------------------------------------- the sound

// The media frame index of the decoded frame's first sample, from its
// timestamp and its stream's own start, or -1 when it has none.
static int64_t frame_index(os64_media_t *m)
{
    int64_t pts = m->frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) return -1;
    AVStream *s = m->format->streams[m->stream];
    if (s->start_time != AV_NOPTS_VALUE) pts -= s->start_time;
    return av_rescale_q(pts, s->time_base, (AVRational){ 1, (int)m->rate });
}

// The decoded sound frame in `m->frame` into the output buffer.
static int sound_frame_into_out(os64_media_t *m)
{
    const AVFrame *f = m->frame;
    // A stream may change rate partway (two MP3s joined): what the old
    // resampler holds is drawn out, and a new one starts.
    if (f->sample_rate > 0 && (uint32_t)f->sample_rate != m->rate) {
        int r = resampler_drain(m);
        if (r < 0) return r;
        m->rate = (uint32_t)f->sample_rate;
        if ((r = resampler_start(m)) < 0) return r;
    }
    // The roles come from each frame's own layout, which may change with
    // the rate. A frame that does not name its channels (a plain PCM WAV of
    // six) is read as the usual layout for that count, 5.1 for six, as
    // FFmpeg's own resampler reads it.
    int channels = f->ch_layout.nb_channels;
    if (channels < 1 || channels > MEDIA_PCM_MAX_CHANNELS) return OS64_MEDIA_UNSUPPORTED;
    AVChannelLayout layout = f->ch_layout;
    if (layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&layout, channels);
    for (int c = 0; c < channels; c++)
        m->roles[c] = role_of(av_channel_layout_channel_from_index(&layout, (unsigned)c));
    media_pcm_in_t in = { .channels = (uint32_t)channels,
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
    return resample_into_out(m, m->stereo + 2 * first, in.frames - first);
}

// One step of the sound: 1 when it moved (the output may have grown; a
// damaged frame skipped counts), 0 when the sound has ended and its tail is
// out, NEED_INPUT when its decoder wants a packet, or an error.
static int sound_step(os64_media_t *m)
{
    if (m->silence > 0) {
        // The sound's lead, before its first frame: silence, a block at a time.
        uint32_t n = m->silence > 4096 ? 4096 : (uint32_t)m->silence;
        if (!grow(&m->out, &m->out_cap, m->out_len + n)) return OS64_MEDIA_NO_MEMORY;
        os64_memset(m->out + 2 * m->out_len, 0, (size_t)n * 4);
        m->out_len += n;
        m->silence -= n;
        return 1;
    }
    if (!m->decoding_done) {
        int r = avcodec_receive_frame(m->codec, m->frame);
        if (r == 0) {
            r = sound_frame_into_out(m);
            return r < 0 ? r : 1;
        }
        // Damaged sound is skipped, as players do, wherever the decoder
        // finds it: in a packet it was sent, or in the rest of one it was
        // still working through. It says so in the error text.
        if (r == AVERROR_INVALIDDATA) return 1;
        if (r == AVERROR(EAGAIN) && !input_ended(m, KIND_SOUND)) return NEED_INPUT;
        // The end, or a decoder that never reports its end once its input has.
        if (r != AVERROR_EOF && r != AVERROR(EAGAIN)) return OS64_MEDIA_DECODE;
        m->decoding_done = true;
        return 1;
    }
    if (!m->tail_done) {
        m->tail_done = true;     // the song is in: what the resampler holds comes out
        int r = resampler_drain(m);
        return r < 0 ? r : 1;
    }
    return 0;
}

// Makes more output, or learns there is none: the sound alone, for
// os64_media_decode, whose media has no picture to read. 1: output was
// made; 0: the media has ended; negative: an error.
static int refill(os64_media_t *m)
{
    m->out_at = m->out_len = 0;
    while (m->out_len == 0) {
        int r = sound_step(m);
        if (r == 0) return 0;
        if (r == NEED_INPUT) {
            r = feed(m, KIND_SOUND);
            if (r == NEED_INPUT) r = read_packet(m);
        }
        if (r < 0) return r;
    }
    return 1;
}

int64_t os64_media_decode(os64_media_t *m, int16_t *out, uint32_t frames)
{
    if (m == NULL || m->codec == NULL || m->want != OS64_MEDIA_WANT_SOUND || (out == NULL && frames > 0))
        return OS64_MEDIA_BAD_ARGS;
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

// ---------------------------------------------------------------- the picture

static void picture_free(media_picture_t *p)
{
    if (p == NULL) return;
    av_frame_free(&p->frame);
    os64_free(p);
}

// What any thread released, back to the decoder's pool: on the decoding
// thread only (§ THREADS).
static void drain_released(os64_media_t *m)
{
    os64_lock_acquire(&m->release_lock);
    media_picture_t *list = m->released;
    m->released = NULL;
    os64_lock_release(&m->release_lock);
    while (list != NULL) {
        media_picture_t *next = list->next;
        picture_free(list);
        m->pictures_returned++;
        list = next;
    }
}

// Pictures made and not given: a seek's or a close's to drop.
static void drop_unready(os64_media_t *m)
{
    picture_free(m->seek_held);
    m->seek_held = NULL;
    for (int i = 0; i < 2; i++) {
        picture_free(m->ready[i]);
        m->ready[i] = NULL;
    }
}

static void make_ready(os64_media_t *m, media_picture_t *p)
{
    if (m->ready[0] == NULL) m->ready[0] = p;
    else m->ready[1] = p;
}

// The decoded frame in `m->pframe` as a picture, timed; NULL when memory
// ran out.
static media_picture_t *picture_from_frame(os64_media_t *m)
{
    media_picture_t *p = os64_calloc(1, sizeof(*p));
    if (p == NULL) return NULL;
    p->frame = av_frame_alloc();
    if (p->frame == NULL) {
        os64_free(p);
        return NULL;
    }
    av_frame_move_ref(p->frame, m->pframe);
    const AVStream *s = m->format->streams[m->pstream];
    int64_t pts = p->frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = start_of(s);
    p->pub.at = av_rescale_q(pts - start_of(s), s->time_base, kOut) + m->picture_lead;
    if (p->frame->duration > 0)
        p->pub.duration = av_rescale_q(p->frame->duration, s->time_base, kOut);
    else if (m->picture_rate.num > 0 && m->picture_rate.den > 0)
        p->pub.duration = av_rescale(OS64_AUDIO_RATE, m->picture_rate.den, m->picture_rate.num);
    p->pub.width = (uint32_t)p->frame->width;
    p->pub.height = (uint32_t)p->frame->height;
    p->pub.keyframe = (p->frame->flags & AV_FRAME_FLAG_KEY) != 0;
    // The colours it is converted from (os64/media.h, THE COLOURS), written
    // into its frame here, on the decoding thread, so a scaler only reads it.
    AVFrame *f = p->frame;
    if (f->colorspace == AVCOL_SPC_UNSPECIFIED)
        f->colorspace = (f->width >= 1280 || f->height > 576) ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    if (f->color_range == AVCOL_RANGE_UNSPECIFIED)
        f->color_range = AVCOL_RANGE_MPEG;
    return p;
}

// One step of the picture: 1 when it moved (a picture may be ready), 0
// when the picture has ended, NEED_INPUT when its decoder wants a packet,
// or an error.
static int picture_step(os64_media_t *m)
{
    int r = avcodec_receive_frame(m->pcodec, m->pframe);
    if (r == AVERROR_INVALIDDATA) return 1;
    if (r == AVERROR(EAGAIN) && !input_ended(m, KIND_PICTURE)) return NEED_INPUT;
    if (r == AVERROR_EOF || r == AVERROR(EAGAIN)) {
        // The end: a seek's held picture is the one showing at a target
        // past the last picture.
        m->picture_done = true;
        if (m->seek_held != NULL) {
            make_ready(m, m->seek_held);
            m->seek_held = NULL;
        }
        m->seek_picture = -1;
        return 1;
    }
    if (r < 0) return OS64_MEDIA_DECODE;
    media_picture_t *p = picture_from_frame(m);
    if (p == NULL) {
        av_frame_unref(m->pframe);
        return OS64_MEDIA_NO_MEMORY;
    }
    // A picture the container times before the file's start should not
    // exist (the zero is the earliest stream's start); it is dropped.
    if (p->pub.at < 0) {
        m->counts.pictures_before_zero++;
        picture_free(p);
        return 1;
    }
    if (m->seek_picture >= 0) {
        if (p->pub.at <= m->seek_picture) {
            // Before the target, or at it: the latest so far is kept, the
            // one before it dropped.
            if (m->seek_held != NULL) m->counts.pictures_run_in++;
            picture_free(m->seek_held);
            m->seek_held = p;
            return 1;
        }
        m->seek_picture = -1;
        if (m->seek_held != NULL) make_ready(m, m->seek_held);
        m->seek_held = NULL;
    }
    make_ready(m, p);
    return 1;
}

// ---------------------------------------------------------------- one puller

int os64_media_next(os64_media_t *m, uint32_t accept, int16_t *pcm, uint32_t cap, os64_media_got_t *got)
{
    if (m == NULL || got == NULL || accept == 0 ||
        (accept & ~(OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE)) != 0)
        return OS64_MEDIA_BAD_ARGS;
    bool sound = m->codec != NULL && (accept & OS64_MEDIA_WANT_SOUND);
    bool picture = m->pcodec != NULL && (accept & OS64_MEDIA_WANT_PICTURE);
    if (sound && (pcm == NULL || cap == 0)) return OS64_MEDIA_BAD_ARGS;
    os64_memset(got, 0, sizeof(*got));
    drain_released(m);
    for (;;) {
        if (picture && m->ready[0] != NULL) {
            got->kind = OS64_MEDIA_GOT_PICTURE;
            got->picture = &m->ready[0]->pub;
            m->ready[0] = m->ready[1];
            m->ready[1] = NULL;
            m->pictures_given++;
            m->counts.pictures_out++;
            return 0;
        }
        if (sound && m->out_at < m->out_len) {
            uint32_t n = m->out_len - m->out_at;
            if (n > cap) n = cap;
            os64_memcpy(pcm, m->out + 2 * m->out_at, (size_t)n * 4);
            got->kind = OS64_MEDIA_GOT_SOUND;
            got->frames = n;
            got->at = m->position;
            m->out_at += n;
            m->position += n;
            return 0;
        }
        if (m->out_at == m->out_len) m->out_at = m->out_len = 0;
        // Each decoder taken in turn gives what it has, fed from its own
        // list; the demuxer is read only when one of them wants input and
        // its list is empty.
        bool want_read = false, moved = false;
        if (picture && !m->picture_done) {
            int r = picture_step(m);
            if (r == NEED_INPUT && (r = feed(m, KIND_PICTURE)) == NEED_INPUT) want_read = true;
            else if (r < 0) return r;
            else moved = true;
            if (m->ready[0] != NULL) continue;
        }
        if (sound) {
            int r = sound_step(m);
            if (r == NEED_INPUT) {
                if ((r = feed(m, KIND_SOUND)) == NEED_INPUT) want_read = true;
                else if (r < 0) return r;
                else moved = true;
            } else if (r < 0) {
                return r;
            } else if (r == 1) {
                moved = true;
            }
            if (m->out_len > 0) continue;
        }
        if (moved) continue;
        if (!want_read) {
            got->kind = OS64_MEDIA_GOT_END;
            return 0;
        }
        // A kind not being taken whose list is full must be taken before the
        // demuxer reads on (THE PACKET LISTS).
        if (!sound && m->codec != NULL && m->list[KIND_SOUND].bytes >= LIST_BYTES) {
            got->kind = OS64_MEDIA_GOT_FULL;
            got->full = OS64_MEDIA_WANT_SOUND;
            return 0;
        }
        if (!picture && m->pcodec != NULL && m->list[KIND_PICTURE].bytes >= LIST_BYTES) {
            got->kind = OS64_MEDIA_GOT_FULL;
            got->full = OS64_MEDIA_WANT_PICTURE;
            return 0;
        }
        int r = read_packet(m);
        if (r < 0) return r;
    }
}

void os64_media_picture_release(os64_media_t *m, os64_media_picture_t *p)
{
    if (m == NULL || p == NULL) return;
    media_picture_t *own = (media_picture_t *)p;
    os64_lock_acquire(&m->release_lock);
    own->next = m->released;
    m->released = own;
    os64_lock_release(&m->release_lock);
}

int os64_media_picture_skip(os64_media_t *m, int level)
{
    if (m == NULL || m->pcodec == NULL || (level != OS64_MEDIA_SKIP_NONE && level != OS64_MEDIA_SKIP_UNUSED))
        return OS64_MEDIA_BAD_ARGS;
    drain_released(m);
    m->pcodec->skip_frame = level == OS64_MEDIA_SKIP_UNUSED ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    return 0;
}

int os64_media_picture_resync(os64_media_t *m)
{
    if (m == NULL || m->pcodec == NULL) return OS64_MEDIA_BAD_ARGS;
    drain_released(m);
    // What is made and not given is from before the keyframe too, and so
    // is what the decoder holds (H.264 delays pictures it has decoded): it
    // is flushed now, or the end of the stream, arriving before another
    // keyframe, would drain those pictures out after the resync.
    for (int i = 0; i < 2; i++) {
        if (m->ready[i] != NULL) m->counts.pictures_resync++;
        picture_free(m->ready[i]);
        m->ready[i] = NULL;
    }
    if (m->seek_held != NULL) m->counts.pictures_resync++;
    picture_free(m->seek_held);
    m->seek_held = NULL;
    m->seek_picture = -1;
    avcodec_flush_buffers(m->pcodec);
    // Picture packets already read and listed (THE PACKET LISTS) are
    // dropped, undecoded, up to the first keyframe among them, which is
    // where decoding starts again; with none listed, the resync waits for
    // one from the demuxer (read_packet).
    packet_list_t *l = &m->list[KIND_PICTURE];
    while (l->head != NULL && !(l->head->packet->flags & AV_PKT_FLAG_KEY)) {
        AVPacket *p = list_pop(l);
        av_packet_free(&p);
        m->counts.pictures_resync++;
    }
    m->resync = l->head == NULL;
    return 0;
}

void os64_media_counts(const os64_media_t *m, os64_media_counts_t *out)
{
    if (out == NULL) return;
    os64_memset(out, 0, sizeof(*out));
    if (m == NULL) return;
    *out = m->counts;
    out->pictures_held = m->pictures_given - m->pictures_returned;
    out->sound_listed = m->list[KIND_SOUND].packets;
    out->sound_listed_bytes = m->list[KIND_SOUND].bytes;
    out->picture_listed = m->list[KIND_PICTURE].packets;
    out->picture_listed_bytes = m->list[KIND_PICTURE].bytes;
}

// ---------------------------------------------------------------- open, seek, close

// What a failed probe means to a player: the source failed, memory ran
// out, or the bytes are not media this reads.
static int probe_error(int averror)
{
    if (averror == AVERROR(ENOMEM)) return OS64_MEDIA_NO_MEMORY;
    if (averror == AVERROR(EIO)) return OS64_MEDIA_IO;
    return OS64_MEDIA_NOT_MEDIA;
}

// The sound stream: the best this build can decode, or, when it decodes
// none, the best there is (so the info can name it, and the zero is found
// from the same streams whatever was wanted). -1 when the file has none;
// *decoder NULL when it cannot be decoded.
static int sound_stream(AVFormatContext *f, const AVCodec **decoder)
{
    *decoder = NULL;
    int s = av_find_best_stream(f, AVMEDIA_TYPE_AUDIO, -1, -1, decoder, 0);
    if (s == AVERROR_DECODER_NOT_FOUND) {
        *decoder = NULL;
        s = av_find_best_stream(f, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    }
    return s < 0 ? -1 : s;
}

// The picture stream: the first this build decodes, a default one before
// the rest, else the first there is. Cover art (an attached picture, an
// MP3's or an M4A's) is no picture of the media's and is passed over, as
// it must be for the zero too. Chosen from what the headers say alone, so
// every open of a file chooses the same stream: av_find_best_stream also
// ranks by the frames FFmpeg's probe read, and a sound-only open reads none
// of the picture's.
static int picture_stream(AVFormatContext *f, const AVCodec **decoder)
{
    int best = -1, best_rank = -1;
    for (unsigned i = 0; i < f->nb_streams; i++) {
        const AVStream *st = f->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || (st->disposition & AV_DISPOSITION_ATTACHED_PIC))
            continue;
        int rank = (avcodec_find_decoder(st->codecpar->codec_id) != NULL) * 2 +
                   ((st->disposition & AV_DISPOSITION_DEFAULT) != 0);
        if (rank > best_rank) {
            best = (int)i;
            best_rank = rank;
        }
    }
    *decoder = best >= 0 ? avcodec_find_decoder(f->streams[best]->codecpar->codec_id) : NULL;
    return best;
}

// A decoder for stream `s`, opened (under s_lock, § THREADS). 0, or a
// result.
static int open_decoder(os64_media_t *m, int s, const AVCodec *decoder, AVCodecContext **out)
{
    AVStream *stream = m->format->streams[s];
    AVCodecContext *c = avcodec_alloc_context3(decoder);
    if (c == NULL) return OS64_MEDIA_NO_MEMORY;
    c->opaque = m;
    if (avcodec_parameters_to_context(c, stream->codecpar) < 0) {
        avcodec_free_context(&c);
        return OS64_MEDIA_UNSUPPORTED;
    }
    // The packets' clock: without it the decoder cannot move a frame's
    // timestamp past samples it discards (an MP3's start padding), and a
    // seek near the start would drop too little.
    c->pkt_timebase = stream->time_base;
    c->thread_count = 1;
    int r = avcodec_open2(c, decoder, NULL);
    if (r < 0) {
        avcodec_free_context(&c);
        return r == AVERROR(ENOMEM) ? OS64_MEDIA_NO_MEMORY : OS64_MEDIA_UNSUPPORTED;
    }
    *out = c;
    return 0;
}

// Under s_lock (§ THREADS): learn the streams, which opens decoders of
// FFmpeg's own, and open the ones this media decodes with. A picture the
// media does not want is discarded BEFORE find_stream_info, so it reads
// none of its packets (its start is in the MP4's headers, so the zero does
// not move); an unwanted sound only after, because a sound's start is
// learnt from its first packets. Every other stream is discarded too.
//
// FFmpeg's probe asks every stream, discarded or not, for what only its
// packets can tell (a picture's pixel format, its first timestamp), so a
// discarded picture would keep it reading the sound until its analysis
// limit, five seconds by default, inside s_lock and across the network for
// a page's: about twice the bytes the probe read before the picture was
// discarded. With a picture discarded the probe's analysis is held to a
// tenth of a second, which reads what an undiscarded probe did and decodes
// none of the picture (an MP4's sound is described by its header and its
// decoder's open).
static int streams_and_decoders_locked(os64_media_t *m, os64_media_info_t *info)
{
    bool want_sound = m->want & OS64_MEDIA_WANT_SOUND, want_picture = m->want & OS64_MEDIA_WANT_PICTURE;
    bool picture_discarded = false;
    for (unsigned i = 0; i < m->format->nb_streams; i++) {
        AVStream *st = m->format->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO && (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || !want_picture)) {
            st->discard = AVDISCARD_ALL;
            picture_discarded |= st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        }
    }
    if (picture_discarded) m->format->max_analyze_duration = AV_TIME_BASE / 10;
    int r = avformat_find_stream_info(m->format, NULL);
    if (r < 0) return probe_error(r);

    const AVCodec *sound_decoder = NULL, *picture_decoder = NULL;
    int sound = sound_stream(m->format, &sound_decoder);
    int picture = picture_stream(m->format, &picture_decoder);
    find_zero(m, sound, picture);
    m->found[0] = sound;
    m->found[1] = picture;
    for (unsigned i = 0; i < m->format->nb_streams; i++)
        if ((int)i != sound && (int)i != picture) m->format->streams[i]->discard = AVDISCARD_ALL;

    // The sound, only when wanted (unwanted, the info says ABSENT, and the
    // stream is discarded from here on).
    int sound_refusal = 0;
    if (sound >= 0 && want_sound) {
        AVStream *st = m->format->streams[sound];
        const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
        os64_strcopy(info->codec, sizeof(info->codec), sound_decoder ? sound_decoder->name : d ? d->name : "");
        info->sound = OS64_MEDIA_UNPLAYABLE;
        if (sound_decoder != NULL) {
            sound_refusal = open_decoder(m, sound, sound_decoder, &m->codec);
            int channels = m->codec ? m->codec->ch_layout.nb_channels : 0;
            if (sound_refusal == 0 && (m->codec->sample_rate <= 0 || channels < 1 || channels > MEDIA_PCM_MAX_CHANNELS)) {
                avcodec_free_context(&m->codec);
                sound_refusal = OS64_MEDIA_UNSUPPORTED;
            }
        } else {
            sound_refusal = OS64_MEDIA_UNSUPPORTED;
        }
        if (m->codec != NULL) {
            info->sound = OS64_MEDIA_PLAYABLE;
            m->stream = sound;
            m->rate = (uint32_t)m->codec->sample_rate;
            info->rate = m->rate;
            info->channels = (uint32_t)m->codec->ch_layout.nb_channels;
            m->sound_lead = lead_of(m, st);
        }
    }
    if (sound >= 0 && m->codec == NULL) m->format->streams[sound]->discard = AVDISCARD_ALL;

    // The picture, only when wanted (unwanted, the info says ABSENT and the
    // stream was never read).
    int picture_refusal = 0;
    if (picture >= 0 && want_picture) {
        AVStream *st = m->format->streams[picture];
        const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
        os64_strcopy(info->picture_codec, sizeof(info->picture_codec),
                     picture_decoder ? picture_decoder->name : d ? d->name : "");
        info->picture = OS64_MEDIA_UNPLAYABLE;
        // A picture the scaler cannot read is refused here, never at the
        // first conversion.
        enum AVPixelFormat pixels = (enum AVPixelFormat)st->codecpar->format;
        if (picture_decoder == NULL || pixels == AV_PIX_FMT_NONE || !sws_test_format(pixels, 0))
            picture_refusal = OS64_MEDIA_UNSUPPORTED;
        else
            picture_refusal = open_decoder(m, picture, picture_decoder, &m->pcodec);
        if (m->pcodec != NULL) {
            info->picture = OS64_MEDIA_PLAYABLE;
            m->pstream = picture;
            m->picture_lead = lead_of(m, st);
            if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) m->picture_rate = st->avg_frame_rate;
            info->rate_num = (uint32_t)m->picture_rate.num;
            info->rate_den = (uint32_t)m->picture_rate.den;
            info->coded_width = (uint32_t)st->codecpar->width;
            info->coded_height = (uint32_t)st->codecpar->height;
            AVRational aspect = st->sample_aspect_ratio.num > 0 ? st->sample_aspect_ratio
                              : st->codecpar->sample_aspect_ratio;
            info->width = info->coded_width;
            if (aspect.num > 0 && aspect.den > 0)
                info->width = (uint32_t)av_rescale(info->coded_width, aspect.num, aspect.den);
            info->height = info->coded_height;
            const AVPacketSideData *matrix = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                                     st->codecpar->nb_coded_side_data,
                                                                     AV_PKT_DATA_DISPLAYMATRIX);
            if (matrix != NULL && matrix->size >= 9 * sizeof(int32_t)) {
                // av_display_rotation_get answers counterclockwise degrees,
                // and NaN for a matrix that turns nothing (a damaged one),
                // which is no angle.
                double turn = -av_display_rotation_get((const int32_t *)matrix->data);
                if (turn == turn && turn > -1e6 && turn < 1e6) {
                    long whole = (long)(turn < 0 ? turn - 0.5 : turn + 0.5);
                    whole = ((whole % 360) + 360) % 360;
                    if (whole == 90 || whole == 180 || whole == 270) info->rotation = (uint32_t)whole;
                }
            }
        } else {
            st->discard = AVDISCARD_ALL;
        }
    }

    // The open fails only when nothing wanted can be decoded.
    if (m->codec != NULL || m->pcodec != NULL) return 0;
    if (sound_refusal == OS64_MEDIA_NO_MEMORY || picture_refusal == OS64_MEDIA_NO_MEMORY) return OS64_MEDIA_NO_MEMORY;
    if (sound_refusal != 0 || picture_refusal != 0) return OS64_MEDIA_UNSUPPORTED;
    return want_picture ? OS64_MEDIA_NO_PICTURE : OS64_MEDIA_NO_SOUND;
}

int os64_media_open_for(const os64_media_source_t *src, uint32_t want, os64_media_t **out, os64_media_info_t *info)
{
    if (src == NULL || src->read == NULL || out == NULL || want == 0 ||
        (want & ~(OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE)) != 0)
        return OS64_MEDIA_BAD_ARGS;
    *out = NULL;
    os64_media_t *m = os64_calloc(1, sizeof(*m));
    if (m == NULL) return OS64_MEDIA_NO_MEMORY;
    m->src = *src;
    m->want = want;
    m->stream = m->pstream = m->found[0] = m->found[1] = -1;
    m->seek_target = m->seek_picture = -1;
    os64_media_info_t local;
    if (info == NULL) info = &local;
    os64_memset(info, 0, sizeof(*info));

    // § THREADS: the whole open is under s_lock, released before any
    // return (a failure's close takes it again).
    os64_lock_acquire(&s_lock);
    ready_locked();

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
    m->pframe = av_frame_alloc();
    if (m->format == NULL || m->packet == NULL || m->frame == NULL || m->pframe == NULL) goto fail;
    m->format->pb = m->io;
    m->format->opaque = m;
    m->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (src->remote)
        m->format->flags |= AVFMT_FLAG_FAST_SEEK;   // os64_media_source_t's `remote`

    int r = avformat_open_input(&m->format, NULL, NULL, NULL);
    if (r < 0) {
        result = probe_error(r);   // and avformat_open_input freed the context
        goto fail;
    }
    result = streams_and_decoders_locked(m, info);
    if (result < 0) goto fail;
    if (m->codec != NULL && (result = resampler_start(m)) < 0) goto fail;
    m->silence = m->sound_lead;

    // A reader of several kinds names them all ("mov,mp4,m4a,..."): its
    // first name is the container's.
    const char *name = m->format->iformat->name;
    size_t first = 0;
    while (name[first] != '\0' && name[first] != ',' && first + 1 < sizeof(info->format)) first++;
    os64_memcpy(info->format, name, first);
    info->format[first] = '\0';
    info->seekable = src->seek != NULL;
    // The length runs from the zero to the later end of the sound and the
    // picture, decoded or not (so every open of a file agrees); with neither
    // end known, the container's own length.
    info->frames = -1;
    for (int i = 0; i < 2; i++) {
        if (m->found[i] < 0) continue;
        const AVStream *s = m->format->streams[m->found[i]];
        if (s->duration == AV_NOPTS_VALUE) continue;
        int64_t end = av_rescale_q(s->duration, s->time_base, kOut) + lead_of(m, s);
        if (end > info->frames) info->frames = end;
    }
    if (info->frames < 0 && m->format->duration != AV_NOPTS_VALUE)
        info->frames = av_rescale(m->format->duration, OS64_AUDIO_RATE, AV_TIME_BASE);
    os64_lock_release(&s_lock);
    *out = m;
    return 0;

fail:
    os64_lock_release(&s_lock);
    os64_media_close(m);
    if (info != NULL) os64_memset(info, 0, sizeof(*info));
    return result;
}

int os64_media_open(const os64_media_source_t *src, os64_media_t **out, os64_media_info_t *info)
{
    return os64_media_open_for(src, OS64_MEDIA_WANT_SOUND, out, info);
}

static uint64_t gcd(uint64_t a, uint64_t b)
{
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

// A seek starts two things over that keep history, and each needs a run-in
// or its first output is not the song:
//
// - The DECODER (a lossy one): an MP3's frame may draw on bits stored in
//   the frames before it (the bit reservoir, at most 511 bytes back, which
//   is a few frames at the lowest bitrates), and its filters overlap the
//   frame before. So decoding starts ten of its frames earlier (and as much
//   more as the stream says it needs, `seek_preroll`), and what it makes
//   there is dropped before the resampler.
// - The RESAMPLER, whose filter needs input on both sides of each output.
//   It is fed from twice its latency before the target, and its output
//   before the target is counted off with the delay. That start is put on
//   a boundary of the ratio's cycle (147 media frames for 44.1 kHz, which
//   make exactly 160 output frames), so what comes out is sampled at the
//   same instants as an unbroken decode, and equals it.
//
// A target inside the sound's lead is its silence's: the rest of the lead
// comes out first and the sound starts from its own first frame.
//
// With a picture, the demuxer is moved by the picture's stream to the
// keyframe before both the sound's decoding start and the target (an MP4
// moves every stream to that keyframe's time), and the picture decoder
// runs from it; pictures before the target are dropped as they come, but
// for the last, which is held and given first (`seek_held`).
int os64_media_seek(os64_media_t *m, int64_t frame)
{
    if (m == NULL || frame < 0) return OS64_MEDIA_BAD_ARGS;
    if (m->src.seek == NULL) return OS64_MEDIA_CANNOT_SEEK;
    drain_released(m);

    int64_t sound_frame = frame - m->sound_lead;      // output frames from the sound's own start
    int64_t silence = sound_frame < 0 ? -sound_frame : 0;
    if (sound_frame < 0) sound_frame = 0;
    int64_t start = 0, start_out = 0, sound_ts = 0;
    AVStream *s = m->codec ? m->format->streams[m->stream] : NULL;
    if (m->codec != NULL) {
        int r = resampler_start(m);
        if (r < 0) return r;
        uint64_t cycle_in = m->rate / gcd(m->rate, OS64_AUDIO_RATE);
        uint64_t cycle_out = OS64_AUDIO_RATE / gcd(m->rate, OS64_AUDIO_RATE);
        int64_t preroll = m->resampler ? 2 * (int64_t)os64_resampler_latency(m->resampler) : 0;
        int64_t target = av_rescale(sound_frame, m->rate, OS64_AUDIO_RATE);   // in media frames
        start = target - preroll;
        start = start < 0 ? 0 : start / (int64_t)cycle_in * (int64_t)cycle_in;
        start_out = start / (int64_t)cycle_in * (int64_t)cycle_out;          // exact: a whole number of cycles

        const AVCodecDescriptor *codec = avcodec_descriptor_get(s->codecpar->codec_id);
        int64_t decoder_preroll = s->codecpar->seek_preroll;
        if (codec != NULL && (codec->props & AV_CODEC_PROP_LOSSY))
            decoder_preroll += s->codecpar->frame_size > 0 ? 10 * (int64_t)s->codecpar->frame_size : m->rate / 2;
        int64_t decode_from = start - decoder_preroll < 0 ? 0 : start - decoder_preroll;
        sound_ts = av_rescale_q(decode_from, (AVRational){ 1, (int)m->rate }, s->time_base);
        if (s->start_time != AV_NOPTS_VALUE) sound_ts += s->start_time;
    }
    if (m->pcodec != NULL) {
        AVStream *p = m->format->streams[m->pstream];
        int64_t ts = av_rescale_q(frame, kOut, p->time_base) + av_rescale_q(m->zero_ts, m->zero_tb, p->time_base);
        if (s != NULL) {
            int64_t from = av_rescale_q(sound_ts, s->time_base, p->time_base);
            if (from < ts) ts = from;
        }
        if (av_seek_frame(m->format, m->pstream, ts, AVSEEK_FLAG_BACKWARD) < 0) return OS64_MEDIA_CANNOT_SEEK;
        avcodec_flush_buffers(m->pcodec);
    } else if (av_seek_frame(m->format, m->stream, sound_ts, AVSEEK_FLAG_BACKWARD) < 0) {
        return OS64_MEDIA_CANNOT_SEEK;
    }
    av_packet_unref(m->packet);
    // What the lists hold is from the old place.
    list_clear(&m->list[KIND_SOUND]);
    list_clear(&m->list[KIND_PICTURE]);
    m->reading_done = m->flushed[KIND_SOUND] = m->flushed[KIND_PICTURE] = false;
    if (m->codec != NULL) {
        avcodec_flush_buffers(m->codec);
        m->decoding_done = m->tail_done = false;
        m->seek_target = start;                    // media frames before it (the decoder's run-in) are dropped
        if (sound_frame > start_out) m->skip_out += (uint32_t)(sound_frame - start_out);   // the run-in, with the delay
        m->silence = silence;
    }
    m->out_at = m->out_len = 0;
    if (m->pcodec != NULL) {
        drop_unready(m);
        m->picture_done = m->resync = false;
        m->seek_picture = frame;
    }
    m->position = frame;
    return 0;
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
    os64_lock_acquire(&s_lock);          // § THREADS: the whole close
    // Pictures released go back first; one still held is the caller's bug,
    // and its memory is left alone (its frame holds its own reference, which
    // keeps its buffer alive after the decoder is gone).
    drain_released(m);
    drop_unready(m);
    list_clear(&m->list[KIND_SOUND]);
    list_clear(&m->list[KIND_PICTURE]);
    avcodec_free_context(&m->codec);
    avcodec_free_context(&m->pcodec);
    avformat_close_input(&m->format);     // NULL already when open_input failed
    if (m->io) {
        av_freep(&m->io->buffer);
        avio_context_free(&m->io);
    }
    av_packet_free(&m->packet);
    av_frame_free(&m->frame);
    av_frame_free(&m->pframe);
    os64_resampler_destroy(m->resampler);
    if (m->stereo) os64_free(m->stereo);
    if (m->out) os64_free(m->out);
    os64_free(m);
    os64_lock_release(&s_lock);
}

const char *os64_media_strerror(int result)
{
    switch (result) {
    case 0:                      return "no error";
    case OS64_MEDIA_BAD_ARGS:    return "called wrongly";
    case OS64_MEDIA_NOT_MEDIA:   return "not a media file this can read";
    case OS64_MEDIA_UNSUPPORTED: return "media in a form this cannot decode";
    case OS64_MEDIA_NO_SOUND:    return "no sound in it";
    case OS64_MEDIA_DECODE:      return "damaged past decoding";
    case OS64_MEDIA_IO:          return "the file could not be read";
    case OS64_MEDIA_NO_MEMORY:   return "not enough memory";
    case OS64_MEDIA_CANNOT_SEEK: return "cannot seek there";
    case OS64_MEDIA_NO_PICTURE:  return "no picture in it";
    default:                     return "an error with no name";
    }
}

// ---------------------------------------------------------------- what plays

// Each MIME type other browsers answer for, as the libav reader that reads
// it and, where the type implies its codec, that codec. Which of them play
// is libav.so's answer (av_find_input_format, avcodec_find_decoder_by_name),
// so a type whose reader or decoder is not built answers NO.
static const struct { const char *type, *demuxer, *implied; } kTypes[] = {
    {"video/mp4", "mov", NULL},
    {"audio/mpeg", "mp3", "mp3"}, {"audio/mp3", "mp3", "mp3"}, {"audio/mpeg3", "mp3", "mp3"},
    {"audio/x-mpeg", "mp3", "mp3"}, {"audio/x-mp3", "mp3", "mp3"},
    {"audio/wav", "wav", NULL}, {"audio/wave", "wav", NULL}, {"audio/x-wav", "wav", NULL},
    {"audio/vnd.wave", "wav", NULL},
    {"audio/mp4", "mov", NULL}, {"audio/x-m4a", "mov", NULL}, {"audio/aac", "aac", "aac"},
    {"audio/ogg", "ogg", NULL}, {"application/ogg", "ogg", NULL}, {"audio/webm", "matroska", NULL},
    {"audio/flac", "flac", "flac"}, {"audio/x-flac", "flac", "flac"},
};

// A codecs-parameter name (RFC 6381) as the decoders that decode it.
static const struct { const char *codec, *decoders[3]; } kCodecs[] = {
    {"mp3", {"mp3float", "mp3"}}, {"mp4a.69", {"mp3float", "mp3"}}, {"mp4a.6b", {"mp3float", "mp3"}},
    {"mp4a.40.34", {"mp3float", "mp3"}},
    {"1", {"pcm_s16le"}},
    {"aac", {"aac"}}, {"mp4a.40.2", {"aac"}}, {"mp4a.40.5", {"aac"}}, {"mp4a.40.29", {"aac"}},
    {"mp4a.40", {"aac"}}, {"mp4a.67", {"aac"}},
    {"opus", {"opus", "libopus"}}, {"vorbis", {"vorbis", "libvorbis"}}, {"flac", {"flac"}},
    {"avc1", {"h264"}}, {"avc3", {"h264"}},
};

static bool hex_digit(char c)
{
    return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f');
}

// H.264's codec string (RFC 6381 § 3.3): `avc1` or `avc3`, alone or with a
// dot and six hexadecimal digits (profile, constraints, level), as the
// table's own entry name. NULL for anything else, which does not parse.
static const char *avc_name(const char *codec)
{
    if (!((codec[0] | 0x20) == 'a' && (codec[1] | 0x20) == 'v' && (codec[2] | 0x20) == 'c' &&
          (codec[3] == '1' || codec[3] == '3')))
        return NULL;
    const char *name = codec[3] == '1' ? "avc1" : "avc3";
    if (codec[4] == '\0') return name;
    if (codec[4] != '.') return NULL;
    for (int i = 5; i < 11; i++)
        if (!hex_digit(codec[i])) return NULL;
    return codec[11] == '\0' ? name : NULL;
}

static bool decodes(const char *codec)
{
    const char *avc = avc_name(codec);
    if (avc != NULL) codec = avc;
    else if ((codec[0] | 0x20) == 'a' && (codec[1] | 0x20) == 'v' && (codec[2] | 0x20) == 'c')
        return false;            // an H.264 name that does not parse
    for (size_t i = 0; i < sizeof(kCodecs) / sizeof(kCodecs[0]); i++) {
        if (!os64_streq_nocase(kCodecs[i].codec, codec))
            continue;
        for (size_t d = 0; d < 3 && kCodecs[i].decoders[d] != NULL; d++)
            if (avcodec_find_decoder_by_name(kCodecs[i].decoders[d]) != NULL)
                return true;
        return false;
    }
    return false;
}

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// The value of a `codecs=` parameter in `params` (what follows the type's
// first `;`), its quotes removed, into `out`. False when there is none.
static bool codecs_param(const char *params, char *out, size_t cap)
{
    const char *p = params;
    while (*p != '\0') {
        while (*p == ';' || space(*p))
            p++;
        const char *name = p;
        while (*p != '\0' && *p != '=' && *p != ';')
            p++;
        size_t name_len = (size_t)(p - name);
        while (name_len > 0 && space(name[name_len - 1]))
            name_len--;
        if (*p != '=') {
            continue;
        }
        p++;
        while (space(*p))
            p++;
        bool quoted = *p == '"';
        if (quoted)
            p++;
        const char *value = p;
        while (*p != '\0' && (quoted ? *p != '"' : *p != ';'))
            p++;
        size_t value_len = (size_t)(p - value);
        if (quoted && *p == '"')
            p++;
        while (*p != '\0' && *p != ';')
            p++;
        if (name_len == 6 && (name[0] | 0x20) == 'c' && (name[1] | 0x20) == 'o' && (name[2] | 0x20) == 'd' &&
            (name[3] | 0x20) == 'e' && (name[4] | 0x20) == 'c' && (name[5] | 0x20) == 's') {
            if (value_len >= cap)
                value_len = cap - 1;
            os64_memcpy(out, value, value_len);
            out[value_len] = '\0';
            return true;
        }
    }
    return false;
}

int os64_media_type_supported(const char *type)
{
    if (type == NULL)
        return OS64_MEDIA_TYPE_NO;
    char essence[64];
    size_t n = 0;
    const char *p = type;
    while (space(*p))
        p++;
    while (*p != '\0' && *p != ';' && n + 1 < sizeof(essence))
        essence[n++] = *p++;
    while (n > 0 && space(essence[n - 1]))
        n--;
    essence[n] = '\0';
    while (*p != '\0' && *p != ';')
        p++;                                     // an essence longer than any listed
    const char *demuxer = NULL, *implied = NULL;
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]) && demuxer == NULL; i++)
        if (os64_streq_nocase(kTypes[i].type, essence)) {
            demuxer = kTypes[i].demuxer;
            implied = kTypes[i].implied;
        }
    if (demuxer == NULL || av_find_input_format(demuxer) == NULL)
        return OS64_MEDIA_TYPE_NO;
    char codecs[256];
    if (*p != ';' || !codecs_param(p + 1, codecs, sizeof(codecs)))
        return implied == NULL || decodes(implied) ? OS64_MEDIA_TYPE_MAYBE : OS64_MEDIA_TYPE_NO;
    // Every codec named must be one that decodes; an empty list names none.
    bool any = false;
    for (char *c = codecs; *c != '\0';) {
        while (*c == ',' || space(*c))
            c++;
        char *start = c;
        while (*c != '\0' && *c != ',')
            c++;
        char *end = c;
        while (end > start && space(end[-1]))
            end--;
        if (end == start)
            continue;
        char saved = *end;
        *end = '\0';
        bool ok = decodes(start);
        *end = saved;
        if (!ok)
            return OS64_MEDIA_TYPE_NO;
        any = true;
    }
    return any ? OS64_MEDIA_TYPE_PROBABLY : OS64_MEDIA_TYPE_MAYBE;
}

size_t os64_media_type_list(char *out, size_t cap)
{
    size_t need = 0, kept = 0;
    bool whole = true;
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++) {
        // A link to a video is not offered as a sound's page: a video type
        // is no SOUND type.
        if (os64_memcmp(kTypes[i].type, "video/", 6) == 0 ||
            os64_media_type_supported(kTypes[i].type) == OS64_MEDIA_TYPE_NO)
            continue;
        size_t sep = need == 0 ? 0 : 2, len = os64_strlen(kTypes[i].type);
        need += sep + len;
        if (whole && out != NULL && kept + sep + len < cap) {
            if (sep != 0)
                os64_memcpy(out + kept, ", ", 2);
            os64_memcpy(out + kept + sep, kTypes[i].type, len);
            kept += sep + len;
        } else {
            whole = false;
        }
    }
    if (out != NULL && cap > 0)
        out[kept] = '\0';
    return need;
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
    out->remote = false;
    // The media is the whole file: the size and every seek count from its
    // first byte, so reading starts there too.
    int64_t end = os64_seek(handle, 0, OS64_SEEK_END);
    if (end < 0 || os64_seek(handle, 0, OS64_SEEK_SET) != 0) {
        out->seek = NULL;    // not a file with a position: a pipe, the console
        end = -1;
    }
    out->size = end;
}
