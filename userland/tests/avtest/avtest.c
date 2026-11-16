// avtest — libav.so (FFmpeg, ported) in ring 3: the library loads, reads
// its input through a caller's AVIOContext the way libmedia will, and
// decodes. Each check names its step in the exit code.
//
//   1  the library's versions are the headers' (the right libav.so loaded)
//   2  an MP3 in memory opens and probes as MP3, mono, 44.1 kHz
//   3  its ID3v2 tag reads back: title, and genre "(17)" as "Rock", which
//      FFmpeg's id3v2 reader works out with sscanf (the port's routed scanf)
//   4  its decoder opens
//   5  it decodes to exactly 44,100 samples: the Xing header's delay and
//      padding are trimmed, so one second of sine is one second of sound
//   6  the sound is the sine: 440 Hz by its zero crossings, and at the
//      level the host's FFmpeg decodes the same bytes to (sine_mp3.h)
//   7  a seek to half a second lands there, to within one MP3 frame
//   8  a WAV built here decodes sample for sample (PCM is lossless)
//   9  bytes that are no media at all are refused, and nothing breaks
//  10  an MP4 whose first track is a picture no decoder here reads opens
//      as MP4, and its sound (AAC-LC, mono, 44.1 kHz) is the stream found
//  11  the AAC decoder opens
//  12  the sound is one second of the 440 Hz sine, holding the energy the
//      host's FFmpeg decodes the same bytes to (sine_m4a.h)
//
// Exit codes: 0xA7E50000 success, 0xA7E500NN the step that failed.

#include "os64/os64.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/channel_layout.h"
#include "libavutil/dict.h"
#include "libavutil/mem.h"

#include "sine_m4a.h"
#include "sine_mp3.h"

#define PASS 0xA7E50000
#define FAIL(step) ((int)(PASS | (step)))

static void say(const char *what, long a, long b)
{
    os64_printf("avtest: %s: %ld, %ld\n", what, a, b);
}

// FFmpeg's words reach the terminal only when they are errors, so a
// failure says why and a pass is quiet.
static void on_log(void *avcl, int level, const char *fmt, va_list ap)
{
    (void)avcl;
    if (level > AV_LOG_ERROR) return;
    char line[256];
    os64_vsnprintf(line, sizeof(line), fmt, ap);
    os64_printf("avtest: ffmpeg: %s", line);
}

// ------------------------------------------------------------ memory input

typedef struct {
    const uint8_t *data;
    int64_t size, at;
} Memory;

static int memory_read(void *opaque, uint8_t *out, int cap)
{
    Memory *m = opaque;
    int64_t left = m->size - m->at;
    if (left <= 0) return AVERROR_EOF;
    int n = cap < left ? cap : (int)left;
    os64_memcpy(out, m->data + m->at, (size_t)n);
    m->at += n;
    return n;
}

static int64_t memory_seek(void *opaque, int64_t offset, int whence)
{
    Memory *m = opaque;
    if (whence == AVSEEK_SIZE) return m->size;
    whence &= ~AVSEEK_FORCE;
    int64_t to = whence == SEEK_SET ? offset : whence == SEEK_CUR ? m->at + offset
               : whence == SEEK_END ? m->size + offset : -1;
    if (to < 0 || to > m->size) return AVERROR(EINVAL);
    m->at = to;
    return to;
}

typedef struct {
    Memory memory;
    AVIOContext *io;
    AVFormatContext *format;
    AVCodecContext *codec;
    int stream;
} Input;

static void input_close(Input *in)
{
    avcodec_free_context(&in->codec);
    avformat_close_input(&in->format);
    if (in->io) {
        av_freep(&in->io->buffer);
        avio_context_free(&in->io);
    }
}

// Opens and probes; the codec is opened separately so each is its own step.
static int input_open(Input *in, const uint8_t *data, size_t size)
{
    os64_memset(in, 0, sizeof(*in));
    in->memory = (Memory){ .data = data, .size = (int64_t)size };
    enum { IO_BUFFER = 4096 };
    uint8_t *buffer = av_malloc(IO_BUFFER);
    if (!buffer) return AVERROR(ENOMEM);
    in->io = avio_alloc_context(buffer, IO_BUFFER, 0, &in->memory, memory_read, NULL, memory_seek);
    if (!in->io) {
        av_free(buffer);
        return AVERROR(ENOMEM);
    }
    in->format = avformat_alloc_context();
    if (!in->format) return AVERROR(ENOMEM);
    in->format->pb = in->io;
    int r = avformat_open_input(&in->format, NULL, NULL, NULL);
    if (r < 0) return r;          // avformat_open_input freed the context
    r = avformat_find_stream_info(in->format, NULL);
    if (r < 0) return r;
    in->stream = av_find_best_stream(in->format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    return in->stream;
}

static int codec_open(Input *in)
{
    const AVCodecParameters *par = in->format->streams[in->stream]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(par->codec_id);
    if (!decoder) return AVERROR_DECODER_NOT_FOUND;
    in->codec = avcodec_alloc_context3(decoder);
    if (!in->codec) return AVERROR(ENOMEM);
    int r = avcodec_parameters_to_context(in->codec, par);
    if (r < 0) return r;
    return avcodec_open2(in->codec, decoder, NULL);
}

// Hands every frame the decoder has ready to `take`. Returns 0 when it
// wants more input, 1 when the decoder has ended, FFmpeg's error, or what
// `take` returned to stop (2).
static int drain(Input *in, AVFrame *frame, int (*take)(const AVFrame *, void *), void *ctx)
{
    for (;;) {
        int r = avcodec_receive_frame(in->codec, frame);
        if (r == AVERROR(EAGAIN)) return 0;
        if (r == AVERROR_EOF) return 1;
        if (r < 0) return r;
        int stop = take(frame, ctx);
        av_frame_unref(frame);
        if (stop) return stop;
    }
}

// FFmpeg's send/receive contract: a packet the decoder answers EAGAIN to is
// not taken; drain what it has ready and send the SAME packet again. A NULL
// packet starts the decoder's end.
static int send(Input *in, AVPacket *packet, AVFrame *frame, int (*take)(const AVFrame *, void *), void *ctx)
{
    for (;;) {
        int r = avcodec_send_packet(in->codec, packet);
        if (r == AVERROR(EAGAIN)) {
            int d = drain(in, frame, take, ctx);
            if (d != 0) return d;
            continue;
        }
        if (r < 0) return r;
        return drain(in, frame, take, ctx);
    }
}

// Calls `take` with every decoded frame until the input ends; returns 0, or
// FFmpeg's error, or what `take` returned to stop early (2).
static int decode_all(Input *in, int (*take)(const AVFrame *, void *), void *ctx)
{
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int result = packet && frame ? 0 : AVERROR(ENOMEM);
    while (result == 0) {
        int r = av_read_frame(in->format, packet);
        if (r == AVERROR_EOF) {
            result = send(in, NULL, frame, take, ctx);
            if (result == 0) result = drain(in, frame, take, ctx);
            break;
        }
        if (r < 0) { result = r; break; }
        if (packet->stream_index == in->stream) result = send(in, packet, frame, take, ctx);
        av_packet_unref(packet);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    return result == 1 ? 0 : result;
}

// ------------------------------------------------------------ the sine

typedef struct {
    int64_t samples, crossings;
    double sum_squares;
    float last;
    int planar_float;
} Sine;

static int take_sine(const AVFrame *frame, void *ctx)
{
    Sine *s = ctx;
    if (frame->format != AV_SAMPLE_FMT_FLTP) {
        s->planar_float = 0;
        return 2;
    }
    const float *x = (const float *)frame->extended_data[0];
    for (int i = 0; i < frame->nb_samples; i++) {
        if ((s->last < 0) != (x[i] < 0) && s->samples > 0) s->crossings++;
        s->last = x[i];
        s->sum_squares += (double)x[i] * x[i];
        s->samples++;
    }
    return 0;
}

static int take_first_pts(const AVFrame *frame, void *ctx)
{
    *(int64_t *)ctx = frame->pts;
    return 2;
}

static int check_mp3(void)
{
    Input in;
    int r = input_open(&in, kSineMp3, sizeof(kSineMp3));
    const AVCodecParameters *par = r >= 0 ? in.format->streams[in.stream]->codecpar : NULL;
    if (r < 0 || os64_strcmp(in.format->iformat->name, "mp3") != 0
        || par->sample_rate != 44100 || par->ch_layout.nb_channels != 1) {
        say("MP3 probe (stream or error, rate)", r, par ? par->sample_rate : 0);
        input_close(&in);
        return FAIL(2);
    }

    const AVDictionaryEntry *title = av_dict_get(in.format->metadata, "title", NULL, 0);
    const AVDictionaryEntry *genre = av_dict_get(in.format->metadata, "genre", NULL, 0);
    if (!title || !genre || os64_strcmp(title->value, "avtest") != 0
        || os64_strcmp(genre->value, "Rock") != 0) {
        os64_printf("avtest: tag: title '%s' genre '%s'\n", title ? title->value : "(none)",
                    genre ? genre->value : "(none)");
        input_close(&in);
        return FAIL(3);
    }

    if ((r = codec_open(&in)) < 0) {
        say("MP3 decoder open (error)", r, 0);
        input_close(&in);
        return FAIL(4);
    }

    Sine sine = { .planar_float = 1 };
    r = decode_all(&in, take_sine, &sine);
    if (r != 0 || !sine.planar_float || sine.samples != SINE_MP3_SAMPLES) {
        say("MP3 decode (result, samples)", r, (long)sine.samples);
        input_close(&in);
        return FAIL(5);
    }

    // A sine crosses zero twice a cycle. The level is the host decoder's
    // for these bytes, to within 0.1%: the port builds FFmpeg's C path, as
    // the host's measurement did, so anything wider than rounding means the
    // synthesis is wrong, not that MP3 is lossy.
    double hz = (double)sine.crossings / 2.0;
    double ppm = __builtin_sqrt(sine.sum_squares / (double)sine.samples) * 1e6;
    if (hz < 438.0 || hz > 442.0 || ppm < SINE_MP3_RMS_PPM * 0.999 || ppm > SINE_MP3_RMS_PPM * 1.001) {
        say("MP3 sound (crossings, level per million)", (long)sine.crossings, (long)ppm);
        input_close(&in);
        return FAIL(6);
    }

    AVStream *stream = in.format->streams[in.stream];
    int64_t target = av_rescale_q(AV_TIME_BASE / 2, AV_TIME_BASE_Q, stream->time_base);
    int64_t landed = AV_NOPTS_VALUE;
    avcodec_flush_buffers(in.codec);
    r = av_seek_frame(in.format, in.stream, target, AVSEEK_FLAG_BACKWARD);
    if (r >= 0) r = decode_all(&in, take_first_pts, &landed);
    int64_t off = landed == AV_NOPTS_VALUE ? INT64_MAX
                : av_rescale_q(landed - target, stream->time_base, (AVRational){ 1, 44100 });
    if (r < 0 || off < -1152 || off > 1152) {
        say("MP3 seek (result, samples from half a second)", r, (long)off);
        input_close(&in);
        return FAIL(7);
    }
    input_close(&in);
    return 0;
}

// ------------------------------------------------------------ the MP4

static int check_m4a(void)
{
    Input in;
    int r = input_open(&in, kSineM4a, sizeof(kSineM4a));
    const AVCodecParameters *par = r >= 0 ? in.format->streams[in.stream]->codecpar : NULL;
    if (r < 0 || os64_strcmp(in.format->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") != 0 || in.format->nb_streams != 2
        || in.stream != 1
        || par->codec_id != AV_CODEC_ID_AAC || par->sample_rate != 44100 || par->ch_layout.nb_channels != 1) {
        say("MP4 probe (stream or error, rate)", r, par ? par->sample_rate : 0);
        input_close(&in);
        return FAIL(10);
    }
    if ((r = codec_open(&in)) < 0) {
        say("AAC decoder open (error)", r, 0);
        input_close(&in);
        return FAIL(11);
    }
    // The count is bounded, not exact as the MP3's is, and the reference is
    // the sound's energy, not its level: the host's FFmpeg may be another
    // release than the port's, and releases differ in how much of the
    // encoder's priming (near silence) they trim, never more than one AAC
    // frame (1,024 samples), which moves the count and the average level
    // but not the energy.
    Sine sine = { .planar_float = 1 };
    r = decode_all(&in, take_sine, &sine);
    double hz = (double)sine.crossings / 2.0 * 44100.0 / (double)(sine.samples > 0 ? sine.samples : 1);
    double energy = sine.sum_squares * 1e6;
    if (r != 0 || !sine.planar_float || sine.samples < 44100 || sine.samples > SINE_M4A_SAMPLES + 1024
        || hz < 435.0 || hz > 445.0 || energy < SINE_M4A_ENERGY_PPM * 0.995 || energy > SINE_M4A_ENERGY_PPM * 1.005) {
        say("AAC sound (samples, energy per million)", (long)sine.samples, (long)energy);
        input_close(&in);
        return FAIL(12);
    }
    input_close(&in);
    return 0;
}

// ------------------------------------------------------------ the WAV

enum { WAV_FRAMES = 4800 };

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

typedef struct {
    int64_t frames;
    int wrong;
} Ramp;

static int take_ramp(const AVFrame *frame, void *ctx)
{
    Ramp *r = ctx;
    if (frame->format != AV_SAMPLE_FMT_S16 || frame->ch_layout.nb_channels != 2) {
        r->wrong = 1;
        return 2;
    }
    const int16_t *x = (const int16_t *)frame->data[0];
    for (int i = 0; i < frame->nb_samples; i++, r->frames++) {
        int16_t left = (int16_t)(r->frames * 7), right = (int16_t)~left;
        if (x[2 * i] != left || x[2 * i + 1] != right) r->wrong = 1;
    }
    return 0;
}

static int check_wav(void)
{
    static uint8_t wav[44 + WAV_FRAMES * 4];
    os64_memcpy(wav, "RIFF", 4);
    put32(wav + 4, sizeof(wav) - 8);
    os64_memcpy(wav + 8, "WAVEfmt ", 8);
    put32(wav + 16, 16);
    put16(wav + 20, 1);              // PCM
    put16(wav + 22, 2);              // two channels
    put32(wav + 24, 48000);
    put32(wav + 28, 48000 * 4);
    put16(wav + 32, 4);
    put16(wav + 34, 16);
    os64_memcpy(wav + 36, "data", 4);
    put32(wav + 40, WAV_FRAMES * 4);
    for (uint32_t i = 0; i < WAV_FRAMES; i++) {
        int16_t left = (int16_t)(i * 7), right = (int16_t)~left;
        put16(wav + 44 + i * 4, (uint16_t)left);
        put16(wav + 46 + i * 4, (uint16_t)right);
    }

    Input in;
    int r = input_open(&in, wav, sizeof(wav));
    if (r >= 0) r = codec_open(&in);
    Ramp ramp = { 0 };
    if (r >= 0) r = decode_all(&in, take_ramp, &ramp);
    int ok = r >= 0 && os64_strcmp(in.format->iformat->name, "wav") == 0
          && !ramp.wrong && ramp.frames == WAV_FRAMES;
    if (!ok) say("WAV (result, frames)", r, (long)ramp.frames);
    input_close(&in);
    return ok ? 0 : FAIL(8);
}

static int check_garbage(void)
{
    static uint8_t noise[4096];
    uint32_t x = 0x2545F491;
    for (size_t i = 0; i < sizeof(noise); i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        noise[i] = (uint8_t)x;
    }
    Input in;
    int r = input_open(&in, noise, sizeof(noise));
    input_close(&in);
    if (r >= 0) {
        say("noise was taken for media (stream)", r, 0);
        return FAIL(9);
    }
    return 0;
}

int main(void)
{
    av_log_set_callback(on_log);
    if (avformat_version() != LIBAVFORMAT_VERSION_INT || avcodec_version() != LIBAVCODEC_VERSION_INT
        || avutil_version() != LIBAVUTIL_VERSION_INT) {
        say("libav.so is not the headers' build (avformat, expected)", (long)avformat_version(), (long)LIBAVFORMAT_VERSION_INT);
        return FAIL(1);
    }
    int failed = check_mp3();
    if (!failed) failed = check_wav();
    if (!failed) failed = check_garbage();
    if (!failed) failed = check_m4a();
    if (failed) {
        os64_printf("avtest: FAIL step %d\n", failed & 0xFF);
        return failed;
    }
    os64_printf("avtest: FFmpeg %s decodes MP3, WAV and AAC in MP4; all checks passed\n", av_version_info());
    return PASS;
}
