# sbc.mk — how the kernel builds Android's SBC encoder. Included by
# kernel/GNUmakefile, which adds $(SBC_OBJ) to the kernel's objects. The
# sources live outside src/, so the kernel's own find-every-.c rule never
# compiles them with the kernel's flags.
#
# UPSTREAM FILES get rules of their own (README.md § How it is built says
# why each flag is there):
#   - the kernel's code-generation flags, taken from CFLAGS itself so that a
#     flag the kernel adds (an -mno-* above all) reaches this code too: what
#     is filtered out is the optimisation level, the warnings and their
#     -Werror (but not -Wa, -Wl, -Wp), the include paths and the dependency
#     flags; tools/test_sbc_audit.py reads the flags back from the objects;
#   - -O2, because upstream at -O0 costs about three times as much a frame;
#   - -fno-strict-aliasing, because upstream reads its filter history as
#     int16_t through an int32_t array (sbc_analysis.c's s16X over s32X);
#   - -Wall -Wextra without -Werror: upstream's warnings are shown, and are
#     upstream's to fix;
#   - sbc_switches.h force-included, the shim string.h, upstream's headers.
# OUR FILE (port/sbc_port.c) builds with the kernel's CFLAGS whole,
# -Werror and -O0 included, like every other kernel source.
include ports/sbc/sources.mk

SBC_UPSTREAM_OBJ := $(patsubst %.c,obj/%.c.o,$(SBC_UPSTREAM_SRCS))
SBC_PORT_OBJ := $(patsubst %.c,obj/%.c.o,$(SBC_PORT_SRCS))
SBC_OBJ := $(SBC_UPSTREAM_OBJ) $(SBC_PORT_OBJ)

# The kernel's warning flags: every -W... except -Wa, -Wl, and -Wp, which
# pass options to the assembler, linker and preprocessor and are code
# generation's business, not warnings. CFLAGS spells the kernel's own
# include directory as two words ("-I include"), hence the bare `include`
# in the filter.
sbc_comma := ,
SBC_KERNEL_WARNINGS = $(filter-out -Wa$(sbc_comma)% -Wl$(sbc_comma)% -Wp$(sbc_comma)%,$(filter -W%,$(CFLAGS)))
SBC_UPSTREAM_CFLAGS = $(filter-out -O% $(SBC_KERNEL_WARNINGS) -I% include -MMD -MP,$(CFLAGS)) \
    -O2 -fno-strict-aliasing -Wall -Wextra \
    -include ports/sbc/port/sbc_switches.h \
    -Iports/sbc/port/compat -Iports/sbc/upstream/include -MMD -MP

obj/ports/sbc/upstream/%.c.o: ports/sbc/upstream/%.c ports/sbc/port/sbc_switches.h GNUmakefile ports/sbc/sbc.mk ports/sbc/sources.mk
	@mkdir -p "$$(dirname $@)"
	@$(CC) $(SBC_UPSTREAM_CFLAGS) -c $< -o $@

obj/ports/sbc/port/%.c.o: ports/sbc/port/%.c GNUmakefile ports/sbc/sbc.mk
	@mkdir -p "$$(dirname $@)"
	@$(CC) $(CFLAGS) -Iports/sbc/port -Iports/sbc/upstream/include -c $< -o $@
