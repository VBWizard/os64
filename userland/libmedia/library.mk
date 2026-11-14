# libmedia.so — sound files decoded to what /dev/audio takes (os64/media.h).
#
# The one library that calls FFmpeg (libav.so): every program that plays a
# file goes through os64/media.h and never sees FFmpeg, so the decoder
# behind it can change without a caller changing. tools/test_av_audit.py
# holds that line. It resamples with libsound.so.
# The programs that call it.
LIBMEDIA_PROGRAMS := play mediatest
LIBMEDIA_SRCS := libmedia/media.c libmedia/media_pcm.c
LIBMEDIA_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBMEDIA_SRCS))
LIBMEDIA_SO   := $(BIN)/libmedia.so
LIBMEDIA_BASE  = $(patsubst libmedia.so=%,%,$(filter libmedia.so=%,$(LIB_BASE_PAIRS)))
# FFmpeg's headers come with LIBAV_CONSUMER_CFLAGS (libav/shared.mk).
LIBMEDIA_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden $(LIBAV_CONSUMER_CFLAGS)

$(LIBMEDIA_SO): $(LIBMEDIA_OBJS) $(LIBAV_SO) $(LIBSOUND_SO) $(LIBOS64_SO) link/lib.ld tools/app_bases.py libmedia/library.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBMEDIA_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libmedia.so --no-undefined --no-as-needed \
	    -o $@ $(LIBMEDIA_OBJS) $(LIBAV_SO) $(LIBSOUND_SO) $(LIBOS64_SO)

$(OBJ)/pic/libmedia/%.c.o: libmedia/%.c libmedia/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBMEDIA_CFLAGS) -c $< -o $@

-include $(LIBMEDIA_OBJS:.o=.d)
