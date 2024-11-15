# Nuke built-in rules and variables.
MAKEFLAGS += -rR
MAKEFLAGS += -j$(shell nproc)
# Find all directories in `include` recursively
INCLUDE_DIRS := $(shell find include -type d)
# Prefix each directory with -I for GCC
INCLUDE_FLAGS := $(patsubst %, -I%, $(INCLUDE_DIRS))

.SUFFIXES:

# This is the name that our final executable will have.
# Change as needed.
override OUTPUT := os64_kernel

# Convenience macro to reliably declare user overridable variables.
override USER_VARIABLE = $(if $(filter $(origin $(1)),default undefined),$(eval override $(1) := $(2)))

# Destination directory on install (should always be empty by default).
$(call USER_VARIABLE,DESTDIR,)

# Install prefix; /usr/local is a good, standard default pick.
$(call USER_VARIABLE,PREFIX,/usr/local)

# User controllable C compiler command.
$(call USER_VARIABLE,CC,cc)

# User controllable archiver command.
$(call USER_VARIABLE,AR,ar)

# User controllable linker command.
$(call USER_VARIABLE,LD,ld)

# User controllable C flags.
$(call USER_VARIABLE,CFLAGS,-g -O0 -I include -pipe)

# User controllable C preprocessor flags. We set none by default.
$(call USER_VARIABLE,CPPFLAGS,)

# User controllable nasm flags.
$(call USER_VARIABLE,NASMFLAGS,-F dwarf -g)

# User controllable linker flags. We set none by default.
$(call USER_VARIABLE,LDFLAGS,)

# Ensure the dependencies have been obtained.
ifneq ($(shell ( test '$(MAKECMDGOALS)' = clean || test '$(MAKECMDGOALS)' = distclean ); echo $$?),0)
    ifeq ($(shell ( ! test -d freestnd-c-hdrs-0bsd || ! test -d cc-runtime || ! test -f src/limine.h ); echo $$?),0)
        $(error Please run the ./get-deps script first)
    endif
endif

# Check if CC is Clang.
override CC_IS_CLANG := $(shell ! $(CC) --version 2>/dev/null | grep 'clang' >/dev/null 2>&1; echo $$?)

# If the C compiler is Clang, set the target as needed.
ifeq ($(CC_IS_CLANG),1)
    override CC += \
        -target x86_64-unknown-none
endif

# Internal C flags that should not be changed by the user.
override CFLAGS += \
    -Wall \
    -Wextra \
    -std=gnu11 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-PIC \
    -ffunction-sections \
    -fdata-sections \
    -m64 \
    -march=x86-64 \
    -mcmodel=kernel \
	-masm=intel \
	-Werror \
	-Wno-error=unused-but-set-parameter \
	-Wno-error=unused-variable \
	$(INCLUDE_FLAGS)
#    -nostdinc \

# Internal C preprocessor flags that should not be changed by the user.
override CPPFLAGS := \
    -I src \
    -isystem freestnd-c-hdrs-0bsd \
    $(CPPFLAGS) \
    -MMD \
    -MP

# Internal nasm flags that should not be changed by the user.
override NASMFLAGS += \
    -Wall \
    -f elf64

# Internal linker flags that should not be changed by the user.
override LDFLAGS += \
    -m elf_x86_64 \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    -gc-sections \
    -T linker.ld

# Use "find" to glob all *.c, *.S, and *.asm files in the tree and obtain the
# object and header dependency file names.
override CFILES := $(shell cd src && find -L * -type f -name '*.c' | LC_ALL=C sort)
override ASFILES := $(shell cd src && find -L * -type f -name '*.S' | LC_ALL=C sort)
override NASMFILES := $(shell cd src && find -L * -type f -name '*.asm' | LC_ALL=C sort)
override OBJ := $(addprefix obj/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(NASMFILES:.asm=.asm.o))
override HEADER_DEPS := $(addprefix obj/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))

# Default target.
.PHONY: all
all: bin/$(OUTPUT)

# Link rules for building the C compiler runtime.
cc-runtime-x86_64/cc-runtime.a: cc-runtime/*
	rm -rf cc-runtime-x86_64
	cp -r cc-runtime cc-runtime-x86_64
	$(MAKE) -C cc-runtime-x86_64 -f cc-runtime.mk \
		CC="$(CC)" \
		AR="$(AR)" \
		CFLAGS="$(CFLAGS)" \
		CPPFLAGS='-isystem ../freestnd-c-hdrs-0bsd -DCC_RUNTIME_NO_FLOAT'

# Link rules for the final executable.
bin/$(OUTPUT): GNUmakefile linker.ld $(OBJ) cc-runtime-x86_64/cc-runtime.a
	mkdir -p "$$(dirname $@)"
	$(LD) $(OBJ) cc-runtime-x86_64/cc-runtime.a $(LDFLAGS) -o $@
# Include header dependencies.
-include $(HEADER_DEPS)

# Compilation rules for *.c files.
obj/%.c.o: src/%.c GNUmakefile
	mkdir -p "$$(dirname $@)"
	$(CC) $(CFLAGS) -c $< -o $@

# Compilation rules for *.S files.
obj/%.S.o: src/%.S GNUmakefile
	mkdir -p "$$(dirname $@)"
	$(CC) $(CFLAGS) -c $< -o $@

# Compilation rules for *.asm (nasm) files.
obj/%.asm.o: src/%.asm GNUmakefile
	mkdir -p "$$(dirname $@)"
	nasm $(NASMFLAGS) $< -o $@

# Remove object files and the final executable.
.PHONY: clean
clean:
	rm -rf bin obj cc-runtime-x86_64

# Remove everything built and generated including downloaded dependencies.
.PHONY: distclean
distclean: clean
	rm -rf freestnd-c-hdrs-0bsd cc-runtime src/limine.h

# Install the final built executable to its final on-root location.
.PHONY: install
install: all
	install -d "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
	install -m 644 bin/$(OUTPUT) "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/"

# Try to undo whatever the "install" target did.
.PHONY: uninstall
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)"
	-rmdir "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
