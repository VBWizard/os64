# sources.mk — what libsound.so is built from. manifest.json pins each
# upstream file; tools/test_sound_audit.py checks the two agree.

SOUND_UPSTREAM_SRCS := libsound/upstream/libspeexdsp/resample.c
SOUND_PORT_SRCS     := libsound/port/resample.c
