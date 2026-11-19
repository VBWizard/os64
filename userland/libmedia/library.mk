# libmedia.so — media files decoded: sound to what /dev/audio takes,
# pictures to what a window shows (os64/media.h).
#
# The one library that calls FFmpeg (libav.so): every program that plays a
# file goes through os64/media.h and never sees FFmpeg, so the decoder
# behind it can change without a caller changing. tools/test_av_audit.py
# holds that line. It resamples with libsound.so.
# The programs that call it: yonder for a page's media elements.
LIBMEDIA_PROGRAMS := play gplay mediatest yonder
LIBMEDIA_SRCS := libmedia/media.c libmedia/media_scale.c libmedia/media_pcm.c libmedia/media_queue.c libmedia/media_mix.c \
                 libmedia/media_pictures.c libmedia/media_clock.c libmedia/media_pace.c libmedia/media_output.c
LIBMEDIA_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBMEDIA_SRCS))
LIBMEDIA_SO   := $(BIN)/libmedia.so
LIBMEDIA_BASE  = $(patsubst libmedia.so=%,%,$(filter libmedia.so=%,$(LIB_BASE_PAIRS)))
# FFmpeg's headers come with LIBAV_CONSUMER_CFLAGS (libav/shared.mk).
LIBMEDIA_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden $(LIBAV_CONSUMER_CFLAGS)

$(LIBMEDIA_SO): $(LIBMEDIA_OBJS) $(LIBAV_SO) $(LIBSOUND_SO) $(LIBOS64_SO) libmedia/exports.map link/lib.ld tools/app_bases.py libmedia/library.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBMEDIA_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libmedia.so --no-undefined --no-as-needed \
	    --version-script=libmedia/exports.map \
	    -o $@ $(LIBMEDIA_OBJS) $(LIBAV_SO) $(LIBSOUND_SO) $(LIBOS64_SO)

$(OBJ)/pic/libmedia/%.c.o: libmedia/%.c libmedia/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBMEDIA_CFLAGS) -c $< -o $@

-include $(LIBMEDIA_OBJS:.o=.d)
