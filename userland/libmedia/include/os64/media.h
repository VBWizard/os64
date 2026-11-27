#ifndef OS64_MEDIA_H
#define OS64_MEDIA_H

// os64/media.h — media files, decoded: sound to what /dev/audio takes and
// pictures to what a window shows (libmedia.so).
//
// Sound alone, as `play` does:
//
//     os64_media_source_t src;
//     os64_media_file_source(file, &src);
//     os64_media_t *m;
//     os64_media_info_t info;
//     if (os64_media_open(&src, &m, &info) == 0) {
//         int16_t pcm[4096 * 2];
//         int64_t n;
//         while ((n = os64_media_decode(m, pcm, 4096)) > 0)
//             write the n frames to /dev/audio
//         os64_media_close(m);
//     }
//
// Sound and pictures, as a player does: open with os64_media_open_for,
// then one thread pulls with os64_media_next and routes what comes out
// (§ THE PICTURE SIDE, below).
//
// Whatever the file holds (any format libav.so reads, any sample format,
// mono to many channels, any rate), the sound that comes out is the
// stream's own: signed 16-bit, two channels interleaved, 48,000 frames a
// second (os64/audio.h). The media's own rate and channels are in the info.
//
// THE TIMELINE IS THE OUTPUT'S. Every count, position and time here is in
// output frames (48 kHz) from the start of the media, so frame N of the
// sound that comes out is N / 48000 seconds in, which is what a player's
// clock (/dev/audiopos's `heard`) is measured in too, and a picture whose
// time is N is shown with that frame. The resampler's start-up delay is
// removed, so a seek to a frame returns that frame first and a whole
// decode returns the sound to its own end: the media's length, unless the
// picture outlasts it.
//
// THE START IS THE FILE'S: the earliest start of its sound and its picture
// streams, as the container states them (an MP4's edit lists applied),
// whatever the media was opened for. So every open of a file agrees on
// every position and every seek. A sound stream that starts after that
// zero is preceded by silence, so the sound is contiguous from frame 0.
//
// One media object is used by one thread at a time, but for
// os64_media_picture_release, which any thread may call; several objects
// may be used by several threads at once (libmedia/media.c § THREADS says
// how, over a libav built without threads).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#pragma GCC visibility push(default)

typedef struct os64_media os64_media_t;

// Where the bytes come from. `read` fills up to `cap` bytes and answers how
// many, 0 at the end, negative on an error; it may block. `seek` moves to a
// byte offset from the start and answers it, or negative; NULL when the
// source cannot seek (the media then cannot either). `size` is the byte
// length, or -1 when not known; give it when it is known, seekable or not,
// because a lossy file's end padding is trimmed only when its end is known.
// `remote` says reading costs a download: a seek then goes to the byte its
// time is estimated at (a format's table, or its bit rate), as other
// browsers' do, where a local one reads its way to the exact frame (an
// MP3 has no index, so that read is everything before the target).
// `idle` (NULL for none) is never called by libmedia's own reads; it is for
// a caller that reads on the source's behalf: os64/media_player.h's decoding
// thread calls it, on its own thread, at each wake while it waits with
// nothing to read (its queues full, or paused; 200 ms apart at most), so a
// source fetched from the network may keep its transfer moving, or let its
// fetch go and take it up again at its next read.
typedef struct {
    void *ctx;
    int64_t (*read)(void *ctx, uint8_t *out, size_t cap);
    int64_t (*seek)(void *ctx, uint64_t offset);
    int64_t size;
    bool remote;
    void (*idle)(void *ctx);
} os64_media_source_t;

// What the media holds of one kind (os64_media_info_t's `sound` and
// `picture`).
#define OS64_MEDIA_ABSENT      0   // none in the file, or none asked for
#define OS64_MEDIA_PLAYABLE    1   // there, and it is what this media decodes
#define OS64_MEDIA_UNPLAYABLE  2   // there, in a form this cannot decode (the codec still named)

typedef struct {
    char format[16];      // the container, as libav first names it: "mp3", "wav", "mov"
    uint8_t sound;        // OS64_MEDIA_ABSENT, _PLAYABLE or _UNPLAYABLE
    uint8_t picture;      // the same, for the picture
    bool seekable;
    char codec[16];       // the sound's codec: "mp3float", "aac"; "" when sound is ABSENT
    uint32_t rate;        // the sound's own frames a second; 0 unless it is playable
    uint32_t channels;    // the sound's own channel count; 0 unless it is playable
    int64_t frames;       // the media's length in OUTPUT frames, from the start to the later
                          // stream's end, -1 when not known
    char picture_codec[16];          // "h264"; "" when the picture is ABSENT
                                     // (the fields below are 0 unless it is PLAYABLE)
    uint32_t width, height;          // as shown: the stored size, cropped, with the pixel
                                     // aspect applied to the width; BEFORE rotation
    uint32_t coded_width, coded_height;  // the pixels a picture holds (cropped: 1920x1080,
                                         // never the codec's 1088)
    uint32_t rate_num, rate_den;     // pictures a second, as the file states it; 0/0 when not
    uint32_t rotation;               // degrees clockwise the picture is to be turned to be
                                     // shown upright (0, 90, 180, 270), from the stream's
                                     // display matrix; 0 when it has none or names another angle
} os64_media_info_t;

// Results. 0 is success; each refusal has its own number because a player
// says different things for them.
#define OS64_MEDIA_BAD_ARGS     (-1)   // a NULL where something was needed, or a call this media cannot take (each call says when)
#define OS64_MEDIA_NOT_MEDIA    (-2)   // not a format libav.so reads
#define OS64_MEDIA_UNSUPPORTED  (-3)   // a format it reads, holding what was wanted in a form it cannot decode
#define OS64_MEDIA_NO_SOUND     (-4)   // media with no sound in it, when sound was all that was wanted
#define OS64_MEDIA_DECODE       (-5)   // damaged past decoding
#define OS64_MEDIA_IO           (-6)   // the source failed
#define OS64_MEDIA_NO_MEMORY    (-7)
#define OS64_MEDIA_CANNOT_SEEK  (-8)   // the source cannot seek, or refused the place asked for
#define OS64_MEDIA_NO_PICTURE   (-9)   // media with no picture in it, when a picture was wanted and no sound
                                       // was, or both were and neither is there
#define OS64_MEDIA_NO_PLANE     (-10)  // a plane could not be had (os64/media_player.h says why in its facts)
#define OS64_MEDIA_BUSY         (-11)  // the call's resource is taken (each call says which)

// Probe the source, find its sound, and get ready to decode it. On success
// *out is the media and *info (if not NULL) describes it. The picture, if
// the file has one, is discarded at the demuxer: no packet of it is made or
// decoded (its bytes may still pass through the reading buffer, interleaved
// with the sound's). os64_media_open_for(src, OS64_MEDIA_WANT_SOUND, ...).
int os64_media_open(const os64_media_source_t *src, os64_media_t **out, os64_media_info_t *info);

// Up to `frames` frames of stereo 48 kHz sound into `out` (2 * frames
// samples). Returns how many, which may be fewer than asked before the end;
// 0 at the end of the media; a negative result above on an error. Only for
// a media opened for its sound alone (BAD_ARGS otherwise: pulling sound
// alone would throw the pictures away unseen).
int64_t os64_media_decode(os64_media_t *m, int16_t *out, uint32_t frames);

// The next sound os64_media_decode or os64_media_next gives starts at
// output frame `frame`; for a media opened for its picture, the next
// picture out is the one showing at `frame` (the last whose time is at or
// before it), and the decoder starts at the keyframe before. 0, or a
// negative result above. A frame past the end is not refused (a file's
// length is not always known exactly): the position says that frame, and
// the next decode answers the end. A player clamps to the length it shows.
// After the end, a seek makes the media decode again (a replay, a loop).
int os64_media_seek(os64_media_t *m, int64_t frame);

// The output frame the next sound out starts at.
int64_t os64_media_position(const os64_media_t *m);

// The last error libav reported for this media, as one line, or "".
const char *os64_media_error_text(const os64_media_t *m);

// Frees the media. Every picture it gave is released first; one still held
// is the caller's bug, and close never frees its memory, which a holder may
// still read (os64_media_counts' `pictures_held`, read after a last call on
// the decoding thread, says how many are out). A release after close is
// undefined.
void os64_media_close(os64_media_t *m);

// A result's name, for messages: "not a media file this can read", ...
const char *os64_media_strerror(int result);

// Whether media of a MIME type (with its parameters: "audio/mp4;
// codecs=mp4a.40.2") is media this library plays, in HTML's canPlayType
// words: NO when its container or a codec it names is not one libav.so
// was built to read, or a codec it names does not parse; PROBABLY when it
// names its codecs and each is one libav.so decodes; MAYBE when it names
// none. A picture codec counts as decoded when libav.so decodes it: H.264
// (`avc1` and `avc3`, alone or with their six hexadecimal digits). The
// answer is asked of libav.so itself, so a format switched on in its
// configuration answers here with no list edited anywhere else.
#define OS64_MEDIA_TYPE_NO        0
#define OS64_MEDIA_TYPE_MAYBE     1
#define OS64_MEDIA_TYPE_PROBABLY  2
int os64_media_type_supported(const char *type);

// The SOUND types os64_media_type_supported answers other than NO for
// when they name no codecs (a video type is left out), comma-separated as
// an Accept list is ("audio/mpeg, audio/mp3, ..."), into `out`. Answers
// the length the whole list needs, as os64_snprintf does; a list that does
// not fit is cut at a type, never within one.
size_t os64_media_type_list(char *out, size_t cap);

// A source over an open file handle (os64_open's): the whole file, read
// and seeked on that handle, which is moved to the file's start. It stays
// the caller's, to close after the media. A handle with no position (a
// pipe) is read from where it is and cannot seek.
void os64_media_file_source(int32_t handle, os64_media_source_t *out);

// ---------------------------------------------------------------- THE PICTURE SIDE
//
// A player opens a media for what it wants, and ONE thread (the decoding
// thread) pulls it with os64_media_next, which reads and decodes until
// something comes out and says what it was: some sound, a picture, or the
// end. The player routes each to its queue (os64/media_queue.h for the
// sound). An MP4 interleaves its sound and its pictures, so one demuxer
// serves both. The player says each time which kinds it has room for
// (`accept`): one queue full does not stop the other filling, because
// libmedia reads on past the kind not taken and keeps its packets,
// undecoded, until it is taken again, up to a bound (OS64_MEDIA_GOT_FULL).
// So the sound never runs dry because the pictures fell behind, which is
// what a player must protect.
//
// A PICTURE is a handle the player holds from os64_media_next until it
// releases it. It may be handed to another thread through the player's own
// queue and converted there into a window's pixels by a scaler that thread
// owns; the release may come from any thread. The picture's memory goes
// back to the decoder on the decoding thread, at that media's next call
// (§ THREADS in libmedia/media.c says why it has to be there).
//
// A seek leaves the pictures already handed out valid: each holds its own
// memory. They belong to the old place, and the player's queue drops them
// by generation and releases them.

// What a media is opened for. Whatever is not wanted is discarded at the
// demuxer: no packet of it is made or decoded.
#define OS64_MEDIA_WANT_SOUND    (1u << 0)
#define OS64_MEDIA_WANT_PICTURE  (1u << 1)

// os64_media_open, for `want`. The open succeeds with every wanted kind
// that can be decoded, and the info says of each kind whether it is
// absent, playable or there but unplayable (a video whose sound is in a
// codec not built plays silent; one whose picture cannot be decoded, or is
// in pixels the scaler cannot read, plays its sound). It fails only when
// no wanted kind can be decoded: UNSUPPORTED when one was there and could
// not be, NO_SOUND when sound alone was wanted and there is none,
// NO_PICTURE as that result says. Every refusal is decided here, never at
// the first picture.
int os64_media_open_for(const os64_media_source_t *src, uint32_t want, os64_media_t **out, os64_media_info_t *info);

// A decoded picture. Its fields are read-only, and libmedia's own state
// lives past them.
typedef struct os64_media_picture {
    int64_t at;               // when it is shown: output frames from the start
    int64_t duration;         // output frames it is shown for: the file's own, else one
                              // period at the stated rate, else 0 (not known)
    uint32_t width, height;   // its pixels (the info's coded size, unless the stream changed)
    bool keyframe;
} os64_media_picture_t;

#define OS64_MEDIA_GOT_END      0   // nothing more of any kind accepted
#define OS64_MEDIA_GOT_SOUND    1
#define OS64_MEDIA_GOT_PICTURE  2
#define OS64_MEDIA_GOT_FULL     3   // a kind not accepted must be taken first (`full`)

typedef struct {
    int kind;                         // OS64_MEDIA_GOT_*
    uint32_t frames;                  // SOUND: how many frames were put in `pcm`
    int64_t at;                       // SOUND: the output frame of the first
    os64_media_picture_t *picture;    // PICTURE: now the caller's, to release
    uint32_t full;                    // FULL: the kind (OS64_MEDIA_WANT_*) to take before more of
                                      // the others can be read
} os64_media_got_t;

// Reads and decodes until something of a kind in `accept` comes out (a mask
// of OS64_MEDIA_WANT_*: the kinds the caller has room for now): up to `cap`
// frames of stereo 48 kHz sound into `pcm` (2 * cap samples), or one
// picture, or the end, and says which in *got. Sound comes out in pieces,
// contiguous; a picture comes out whole; each kind in the file's order.
//
// A kind not accepted is not decoded: its packets, read on the way to what
// was, are kept, undecoded, and are what that kind's decoder takes first
// when it is accepted again. Once a kind's kept packet data reaches eight
// MiB (some seconds of a high-definition picture, minutes of any sound)
// the answer is GOT_FULL, naming the kind to take before the others can
// go on, and the caller makes room for that kind (or parks) and asks
// again. GOT_END means nothing more will come of the kinds accepted; one
// not accepted may still have more. A kind the media does not decode may
// be in `accept` and is passed over. 0, or a negative result above
// (BAD_ARGS for an empty or unknown `accept`, or for no `pcm` when sound
// is accepted and the media decodes it). Any media may be pulled this way
// (`pcm` NULL and `cap` 0 when sound is not accepted or the media decodes
// none). After GOT_END, a seek makes it decode again.
int os64_media_next(os64_media_t *m, uint32_t accept, int16_t *pcm, uint32_t cap, os64_media_got_t *got);

// The picture back to libmedia. ANY thread, any time after it was handed
// out, exactly once, and not while a conversion of it is running. It costs
// one short lock; the memory goes back to the decoder at the media's next
// os64_media_next, seek, skip, resync or close.
void os64_media_picture_release(os64_media_t *m, os64_media_picture_t *p);

// What the picture decoder leaves out from now on. NONE decodes every
// picture. UNUSED leaves out the pictures no other picture is predicted
// from (H.264's non-reference pictures); what it does decode is exact.
#define OS64_MEDIA_SKIP_NONE    0
#define OS64_MEDIA_SKIP_UNUSED  1
int os64_media_picture_skip(os64_media_t *m, int level);

// Drops pictures, undecoded, until the next keyframe, which is the next
// picture out (none at all if the stream ends first); the sound goes on as
// before. For a decoder far behind. Picture packets already kept (`accept`
// above) are dropped up to the first keyframe among them, where decoding
// starts again; with none kept, the resync waits for one from the file.
// Both answer 0, or BAD_ARGS for a media that decodes no picture.
int os64_media_picture_resync(os64_media_t *m);

// What the media has done, for a player's own report and for tests. On
// the decoding thread.
typedef struct {
    uint64_t sound_packets;          // read from the demuxer for the sound decoder
    uint64_t picture_packets;        // read for the picture decoder, those a resync then dropped
                                     // included (0 when the picture was not wanted)
    uint64_t pictures_out;           // given by os64_media_next
    uint64_t pictures_held;          // given and not yet back with the decoder: those still out,
                                     // and those released since the last call on the decoding thread
    uint64_t pictures_run_in;        // dropped inside a seek's run-in, before its target
    uint64_t pictures_resync;        // dropped by a resync: packets left undecoded, pictures made and
                                     // not given (not those inside the decoder, which the resync flushes)
    uint64_t pictures_before_zero;   // dropped: timed before the file's start
    uint32_t sound_listed, picture_listed;  // packets kept, read and not yet decoded (`accept`)
    uint64_t sound_listed_bytes, picture_listed_bytes;
} os64_media_counts_t;
void os64_media_counts(const os64_media_t *m, os64_media_counts_t *out);

// A converter into XRGB8888 pixels (os64/gui.h's canvas format; BGRA in
// memory) of dst_width x dst_height. It takes pictures of any size and of
// any kind a media opened (it adapts when one changes), and converts and
// scales in one step. It belongs to one thread at a time, which need not
// be the decoding thread, and to no media.
//   EXACT: the same pixels on every CPU, whichever routines FFmpeg chose
//          (its bit-exact mode; slower): for proofs, never for playing
//   FAST:  FFmpeg's fast bilinear when shrinking (a two-tap filter:
//          cheaper, coarser, and below half the width it skips source
//          pixels); bilinear when enlarging, where at some widths
//          FFmpeg's fast path would run code it writes into memory os64
//          does not let execute
// Neither flag: bilinear.
typedef struct os64_media_scaler os64_media_scaler_t;
#define OS64_MEDIA_SCALE_EXACT  (1u << 0)
#define OS64_MEDIA_SCALE_FAST   (1u << 1)
int os64_media_scaler_create(uint32_t dst_width, uint32_t dst_height, uint32_t flags, os64_media_scaler_t **out);

// `p` into `pixels`: dst_height rows of dst_width pixels, `pitch` bytes
// apart. Reads the picture and writes those pixels and no others; the X
// byte is written 0xFF, so it reads as opaque if it ever becomes alpha. 0;
// UNSUPPORTED for a picture whose kind changed to one the scaler cannot
// read; NO_MEMORY.
//
// AN ODD WIDTH costs FFmpeg four to five times an even one, so a picture
// converted to an odd width other than its own is converted one pixel
// narrower and its last column repeated (1/dst_width narrower, which no
// one can see); not with EXACT, which is always FFmpeg's own answer
// (media_scale.c, THE ODD WIDTH).
//
// THE COLOURS: the matrix and range the stream states; with no matrix,
// BT.709 for a picture 1280 or more wide or taller than 576, else BT.601
// (mpv's rule: standard definition was 601's); with no range, limited
// ("TV"). The full-range JPEG pixel kinds say full range themselves. No
// primaries or transfer conversion is asked for: the pixels keep the
// stream's gamut and transfer, a wide one (BT.2020, PQ, HLG) included. (A
// picture carrying HDR mastering metadata is the exception the scaler
// makes: it maps that picture's luminance range, libswscale/format.c.)
int os64_media_scaler_convert(os64_media_scaler_t *s, const os64_media_picture_t *p, uint32_t *pixels,
                              uint32_t pitch);

// Which way the last conversion that succeeded went, for a readout: the picture at its own
// size (FFmpeg's unscaled converters), bilinear, or fast bilinear (FAST,
// shrinking); NONE before the first, and for a NULL scaler. It is the way
// asked of FFmpeg at the conversion's own size, so a picture one pixel
// narrower than an odd width (AN ODD WIDTH, above) reads as UNSCALED; and
// fast bilinear at a width of eight or less, which libswscale itself
// makes plain bilinear, still reads as fast.
#define OS64_MEDIA_SCALED_NONE          0
#define OS64_MEDIA_SCALED_UNSCALED      1
#define OS64_MEDIA_SCALED_BILINEAR      2
#define OS64_MEDIA_SCALED_FAST_BILINEAR 3
int os64_media_scaler_last(const os64_media_scaler_t *s);

void os64_media_scaler_destroy(os64_media_scaler_t *s);

#pragma GCC visibility pop

#endif // OS64_MEDIA_H
