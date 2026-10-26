# sources.mk — the translation units libfreetype.so is built from.
#
# UPSTREAM: one file per module, because that is upstream's own supported
# single-object build (`upstream/docs/INSTALL.ANY`) — `src/cff/cff.c` includes
# every other file in `src/cff/`, and the `#ifdef`s inside them decide what
# survives port/os64_ftoption.h. Adding a module here means importing its whole
# source directory in tools/import_freetype.py; the two lists answer to each
# other, and a mismatch is a link error rather than a silent half-build.
#
# `base/ftsystem.c` is upstream's platform layer and is DELIBERATELY ABSENT:
# port/ftsystem.c stands in its place so no process-wide heap and no file
# reader can reach the library. `base/ftbbox.c` and `ftmm.c` are absent:
# the adapters do not call those entry points. The stroker's glyph helpers
# require ftglyph.c, which in turn requires ftbitmap.c.
FREETYPE_UPSTREAM_SRCS := \
    libfreetype/upstream/src/autofit/autofit.c \
    libfreetype/upstream/src/base/ftbase.c \
    libfreetype/upstream/src/base/ftdebug.c \
    libfreetype/upstream/src/base/ftinit.c \
    libfreetype/upstream/src/base/ftstroke.c \
    libfreetype/upstream/src/base/ftglyph.c \
    libfreetype/upstream/src/base/ftbitmap.c \
    libfreetype/upstream/src/cff/cff.c \
    libfreetype/upstream/src/psaux/psaux.c \
    libfreetype/upstream/src/pshinter/pshinter.c \
    libfreetype/upstream/src/psnames/psnames.c \
    libfreetype/upstream/src/sfnt/sfnt.c \
    libfreetype/upstream/src/smooth/smooth.c \
    libfreetype/upstream/src/truetype/truetype.c

# THE PORT: backend.c and geometry.c expose separate operation tables. runtime.c and
# nonlocal.S are the C runtime a leaf library has to carry, ftsystem.c is the
# refusal that replaces upstream's platform layer.
FREETYPE_PORT_SRCS := \
    libfreetype/port/backend.c \
    libfreetype/port/geometry.c \
    libfreetype/port/ftsystem.c \
    libfreetype/port/runtime.c \
    libfreetype/port/nonlocal.S
