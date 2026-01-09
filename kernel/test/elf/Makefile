OUTPUT ?= ../../bin/test_elf

CC ?= x86_64-elf-gcc
LD ?= x86_64-elf-ld

CFLAGS ?= -g -m64 -ffreestanding -fno-pic -fno-pie -nostdlib -nostartfiles -nodefaultlibs -Wall -Wextra
LDFLAGS ?= -static -nostdlib -T linker.ld -z max-page-size=0x1000

OBJS := serial_ping.o

.PHONY: all
all: $(OUTPUT)

$(OUTPUT): $(OBJS)
	@mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

serial_ping.o: serial_ping.S
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(OBJS) $(OUTPUT)
