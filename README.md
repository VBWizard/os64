# os64

A 64-bit x86 operating system built for educational purposes: a Limine-booted
higher-half kernel, a ring-3 userland dynamically linked against its own
libraries, a shell (`husk`), a windowing system, a TCP/IP stack with TLS, and a
web browser (`yonder`). It boots on QEMU and on real hardware, BIOS or UEFI.

Where to read next:

*   [CLAUDE.md](CLAUDE.md): how the code works, subsystem by subsystem, and
    the rules the project has learned the hard way.
*   [SUCCESSION.md](SUCCESSION.md): how the project works, and the QEMU
    verification harness.
*   [TOOLCHAIN.md](TOOLCHAIN.md): building the `x86_64-elf` cross compiler.
*   [docs/design/completed/OS64GET.md](docs/design/completed/OS64GET.md):
    installing builds onto a running os64 machine over the network
    (`os64get` and `tools/os64serve.py`).
*   [docs/APPEARANCE_PACKS.md](docs/APPEARANCE_PACKS.md): rebuilding appearance
    packs, keeping versioned host archives, and transferring and selecting them.
*   [DEBTS.md](DEBTS.md), [DECLINED.md](DECLINED.md),
    [DIVERGENCES.md](DIVERGENCES.md): what is deferred, what was declined,
    and where os64 departs from Unix on purpose.

## Getting Started

### Prerequisites

**The cross toolchain.** The kernel and userland are built with
`x86_64-elf-gcc` and `x86_64-elf-ld`, GCC 14.2 and Binutils 2.43, built from
source and installed under `~/opt/cross` with `~/opt/cross/bin` on `PATH`.
[TOOLCHAIN.md](TOOLCHAIN.md) has the configure lines and a script that does
it in about fifteen minutes. The host compiler is not a substitute: the two
toolchains differ in defaults the loader depends on, and the makefiles refuse
to fall back silently.

**Host packages.** On Debian or Ubuntu:

```bash
sudo apt install build-essential make python3 git curl \
                 qemu-system-x86 xorriso e2fsprogs mtools gdisk
```

What each is for: `build-essential` compiles Limine's host tool and the
host-side test suites (the kernel's own assembly is GNU `.S`, assembled by
the cross toolchain, so no separate assembler is needed); `python3`
runs the build's generators (link bases, the ext2 image contents, the
JavaScript engine's prepared sources); `git` and `curl` fetch the pinned
dependencies on first build; `qemu-system-x86` runs it; `xorriso` packs the
ISO; `e2fsprogs` (`mkfs.ext2`, `debugfs`, `e2fsck`), `mtools` (`mformat`,
`mcopy`) and `gdisk` (`sgdisk`) build the disk images without root or loop
devices.

**Optional.** `ccache` makes clean builds fast: the kernel and userland
builds put it in front of the cross compiler when it is on `PATH` and do
nothing when it is not. It exists because `quickjs.c` is a pinned 61,000-line upstream file that
takes 30 to 40 seconds to compile and never changes, so every clean build was
paying for identical bytes. The first line of each build says which compiler
command it chose. The cache lives in `~/.cache/ccache` (5 GB cap, pruned by
itself; `ccache -s` shows hits, misses and size). Entries are keyed by the
compiler's exact command line, which carries the tree's absolute path, so
each worktree warms its own at about 20 MB a tree. If a cached object is
ever suspect: `CCACHE_DISABLE=1 make` bypasses the cache for one build,
`CCACHE_RECACHE=1 make` recompiles and overwrites the stored entries, and
`ccache -C` empties it. `gdb` for debugging, and `bear` for `make compdb`
(the `compile_commands.json` VS Code's IntelliSense reads).

### Building

```bash
git clone git@github.com:VBWizard/os64.git
cd os64
make
```

The first `make` does everything a fresh clone needs, with no separate setup
step: it fetches the pinned freestanding headers and C runtime, clones and
builds Limine (the `v8.x-binary` branch), builds the kernel and the userland
in parallel across every core, builds the disk image (a FAT lifeboat
partition and the ext2 root, both populated from the build), creates the
`/home` data disk, and packs `os64_kernel.iso`. Later builds are incremental.

What it leaves behind:

| File | What it is |
|---|---|
| `os64_kernel.iso` | The bootable ISO: Limine, the kernel, and the disk image as a Limine module (the `RAMDISK` boot entry boots from it alone) |
| `disk/os64.img` | The system disk: FAT lifeboat plus ext2 root, `/bin`, `/lib`, `/tests`, `/etc`. Rebuilt whenever anything on it changes |
| `disk/os64_data.img` | The `/home` disk, 1 GB sparse ext2. Created once, never rebuilt, and removed only by `make distclean` |
| `kernel/bin/os64_kernel` | The kernel ELF, which is what you give GDB |

### Running

```bash
make run          # BIOS boot in QEMU
make run-uefi     # UEFI boot (downloads an OVMF firmware image once)
make run-hdd      # boot from a hard-disk image instead of the ISO
make run-net      # as run, with a virtio NIC on QEMU's user-mode NAT and a pcap of the wire
```

QEMU opens a window with the Limine menu; the default entry boots the ext2
root and lands in `husk`. Both disks are attached. The serial port is written
to `qemu_com1.log`, and a monitor listens on `telnet 127.0.0.1 55555`. The
QEMU command line lives in the root `GNUmakefile` under `QEMUFLAGS`.

### Debugging

```bash
make debug
```

Starts QEMU with its GDB server on port 1234 and the CPU halted at reset.
Then, from the repository root:

```bash
gdb kernel/bin/os64_kernel
(gdb) target remote :1234
```

The `.gdbinit` in the root loads symbols for every userland program and
library at the fixed addresses the build assigned them, so you can break in
`husk` or `libos64.so` as readily as in the kernel.

### Other targets

*   `make clean`: remove build artifacts (kernel and userland objects and
    binaries).
*   `make distclean`: back to a fresh clone, including the fetched
    dependencies, Limine, OVMF, and the `/home` data disk.
*   `make disk`: rebuild `disk/os64.img` from scratch.
*   `make fsck-ext2`: run `e2fsck` over the ext2 root the build produced.
    With a writable root this is the constitution; it must stay green.
*   `make compdb`: regenerate `compile_commands.json` with `bear` (a full
    rebuild).
*   `make -C kernel`, `make -C userland`: build either half on its own.

### Running in VirtualBox

The VirtualBox VM on the Windows side boots its DVD from
`C:/temp/os64_kernel.iso` and uses a fixed-format VDI as its NVMe disk.
Neither is refreshed by a plain `make`. After a build:

```bash
make copy         # hand the fresh ISO to C:/temp (loud if that directory is missing)
make vbox-sync    # copy the system disk into the VDI; the VM must be powered off
```

`make copy` is the kernel half and is cheap; `make vbox-sync` is the disk half,
needed only when the disk contents changed (new programs, libraries, config).
The VDI is a fixed-format image, so the sync is a single `dd` into its data
area and the medium UUID VirtualBox has registered is untouched. Recreating
the VDI, needed only if `DISK_SIZE_MB` changes, is documented above the
`vbox-sync` target in the root `GNUmakefile`.
