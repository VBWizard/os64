WEBP_CORE_SRCS := libwebp/upstream/src/dec/alpha_dec.c \
    libwebp/upstream/src/dec/buffer_dec.c \
    libwebp/upstream/src/dec/frame_dec.c \
    libwebp/upstream/src/dec/idec_dec.c \
    libwebp/upstream/src/dec/io_dec.c \
    libwebp/upstream/src/dec/quant_dec.c \
    libwebp/upstream/src/dec/tree_dec.c \
    libwebp/upstream/src/dec/vp8_dec.c \
    libwebp/upstream/src/dec/vp8l_dec.c \
    libwebp/upstream/src/dec/webp_dec.c \
    libwebp/upstream/src/dsp/alpha_processing.c \
    libwebp/upstream/src/dsp/dec.c \
    libwebp/upstream/src/dsp/dec_clip_tables.c \
    libwebp/upstream/src/dsp/filters.c \
    libwebp/upstream/src/dsp/lossless.c \
    libwebp/upstream/src/dsp/rescaler.c \
    libwebp/upstream/src/dsp/upsampling.c \
    libwebp/upstream/src/dsp/yuv.c \
    libwebp/upstream/src/utils/bit_reader_utils.c \
    libwebp/upstream/src/utils/color_cache_utils.c \
    libwebp/upstream/src/utils/filters_utils.c \
    libwebp/upstream/src/utils/huffman_utils.c \
    libwebp/upstream/src/utils/quant_levels_dec_utils.c \
    libwebp/upstream/src/utils/rescaler_utils.c \
    libwebp/upstream/src/utils/random_utils.c \
    libwebp/upstream/src/utils/thread_utils.c \
    libwebp/upstream/src/utils/utils.c
WEBP_SSE2_SRCS := libwebp/upstream/src/dsp/alpha_processing_sse2.c \
    libwebp/upstream/src/dsp/dec_sse2.c \
    libwebp/upstream/src/dsp/filters_sse2.c \
    libwebp/upstream/src/dsp/lossless_sse2.c \
    libwebp/upstream/src/dsp/rescaler_sse2.c \
    libwebp/upstream/src/dsp/upsampling_sse2.c \
    libwebp/upstream/src/dsp/yuv_sse2.c
