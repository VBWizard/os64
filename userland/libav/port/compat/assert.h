#ifndef OS64_AV_ASSERT_H
#define OS64_AV_ASSERT_H
/* Private to the FFmpeg port: built NDEBUG-style, as FFmpeg's release
 * builds are; FFmpeg's own checks are av_assert0, which stays. */
#define assert(x) ((void)0)
#define static_assert _Static_assert
#endif
