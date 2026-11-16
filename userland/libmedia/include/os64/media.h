#ifndef OS64_MEDIA_H
#define OS64_MEDIA_H

// os64/media.h — sound files, decoded to what /dev/audio takes (libmedia.so).
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
// Whatever the file holds (any format libav.so reads, any sample format,
// mono to many channels, any rate), what comes out is the stream's own:
// signed 16-bit, two channels interleaved, 48,000 frames a second
// (os64/audio.h). The media's own rate and channels are in the info.
//
// THE TIMELINE IS THE OUTPUT'S. Every count and position here is in output
// frames (48 kHz) from the start of the media, so frame N of what
// os64_media_decode returns is N / 48000 seconds into the song, which is
// what a player's clock (/dev/audiopos's `heard`) is measured in too. The
// resampler's start-up delay is removed, so a seek to a frame returns that
// frame first and a whole decode returns the media's length.
//
// One media object is used by one thread at a time; several objects may be
// used by several threads at once (libmedia/media.c § THREADS says how,
// over a libav built without threads).

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
typedef struct {
    void *ctx;
    int64_t (*read)(void *ctx, uint8_t *out, size_t cap);
    int64_t (*seek)(void *ctx, uint64_t offset);
    int64_t size;
    bool remote;
} os64_media_source_t;

typedef struct {
    char format[16];      // the container, as libav first names it: "mp3", "wav", "mov"
    char codec[16];       // the sound inside: "mp3float", "pcm_s16le"
    uint32_t rate;        // the media's own frames a second
    uint32_t channels;    // the media's own channel count
    int64_t frames;       // its length in OUTPUT frames, -1 when not known
    bool seekable;
} os64_media_info_t;

// Results. 0 is success; each refusal has its own number because a player
// says different things for them.
#define OS64_MEDIA_BAD_ARGS     (-1)   // a NULL where something was needed
#define OS64_MEDIA_NOT_MEDIA    (-2)   // not a format libav.so reads
#define OS64_MEDIA_UNSUPPORTED  (-3)   // a format it reads, holding sound it cannot decode
#define OS64_MEDIA_NO_SOUND     (-4)   // media with no sound in it
#define OS64_MEDIA_DECODE       (-5)   // the sound is damaged past decoding
#define OS64_MEDIA_IO           (-6)   // the source failed
#define OS64_MEDIA_NO_MEMORY    (-7)
#define OS64_MEDIA_CANNOT_SEEK  (-8)   // the source cannot seek, or refused the place asked for

// Probe the source, find its sound, and get ready to decode it. On success
// *out is the media and *info (if not NULL) describes it.
int os64_media_open(const os64_media_source_t *src, os64_media_t **out, os64_media_info_t *info);

// Up to `frames` frames of stereo 48 kHz sound into `out` (2 * frames
// samples). Returns how many, which may be fewer than asked before the end;
// 0 at the end of the media; a negative result above on an error.
int64_t os64_media_decode(os64_media_t *m, int16_t *out, uint32_t frames);

// The next os64_media_decode starts at output frame `frame`. 0, or a
// negative result above. A frame past the end is not refused (a file's
// length is not always known exactly): the position says that frame, and
// the next decode answers 0, the end. A player clamps to the length it
// shows.
int os64_media_seek(os64_media_t *m, int64_t frame);

// The output frame the next os64_media_decode starts at.
int64_t os64_media_position(const os64_media_t *m);

// The last error libav reported for this media, as one line, or "".
const char *os64_media_error_text(const os64_media_t *m);

void os64_media_close(os64_media_t *m);

// A result's name, for messages: "not a sound file this can read", ...
const char *os64_media_strerror(int result);

// Whether media of a MIME type (with its parameters: "audio/mp4;
// codecs=mp4a.40.2") is media this library plays, in HTML's canPlayType
// words: NO when its container or a codec it names is not one libav.so
// was built to read; PROBABLY when it names its codecs and each is one
// libav.so decodes; MAYBE when it names none. The answer is asked of
// libav.so itself, so a format switched on in its configuration answers
// here with no list edited anywhere else.
#define OS64_MEDIA_TYPE_NO        0
#define OS64_MEDIA_TYPE_MAYBE     1
#define OS64_MEDIA_TYPE_PROBABLY  2
int os64_media_type_supported(const char *type);

// The SOUND types os64_media_type_supported answers other than NO for
// when they name no codecs (a video type, which plays only its sound, is
// left out), comma-separated as an Accept list is ("audio/mpeg,
// audio/mp3, ..."), into `out`. Answers the length the whole list needs,
// as os64_snprintf does; a list that does not fit is cut at a type, never
// within one.
size_t os64_media_type_list(char *out, size_t cap);

// A source over an open file handle (os64_open's): the whole file, read
// and seeked on that handle, which is moved to the file's start. It stays
// the caller's, to close after the media. A handle with no position (a
// pipe) is read from where it is and cannot seek.
void os64_media_file_source(int32_t handle, os64_media_source_t *out);

#pragma GCC visibility pop

#endif // OS64_MEDIA_H
