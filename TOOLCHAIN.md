# Toolchain

The os64 cross toolchain. Written down on 2026-10-04 after the original install was lost
with the rest of the home directory and nobody could remember what it was.

| Component | Version | Source |
|-----------|---------|--------|
| Binutils  | 2.43    | https://ftp.gnu.org/gnu/binutils/binutils-2.43.tar.xz |
| GCC       | 14.2.0  | https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz |

Target `x86_64-elf`, C only, installed to `~/opt/cross`. `~/.profile` puts
`~/opt/cross/bin` on `PATH`; the makefiles find `x86_64-elf-gcc` and `x86_64-elf-ld`
there (see the CC/LD note in DEBTS.md for why the root GNUmakefile's `-rR` matters).

Versions were established from the `.comment` section of a built kernel (GCC 14.2.0)
and the version string of the one surviving `x86_64-elf-as` binary (Binutils 2.43).

## Rebuild

`~/src/toolchain/build-cross.sh` does the whole thing with no sudo, about 15 minutes
on 24 cores. It downloads both tarballs, verifies their GNU signatures, and installs
to `~/opt/cross`. The configure lines:

```
binutils: ../binutils-2.43/configure --target=x86_64-elf --prefix=$HOME/opt/cross \
          --with-sysroot --disable-nls --disable-werror
gcc:      ../gcc-14.2.0/configure --target=x86_64-elf --prefix=$HOME/opt/cross \
          --disable-nls --enable-languages=c --without-headers --disable-hosted-libstdcxx
          make all-gcc all-target-libgcc; make install-gcc install-target-libgcc
```

### libgcc without the red zone

The kernel builds with `-mno-red-zone`. A stock libgcc assumes the red zone, so the
script adds a multilib before building GCC: `gcc/config/i386/t-x86_64-elf` containing

```
MULTILIB_OPTIONS += mno-red-zone
MULTILIB_DIRNAMES += no-red-zone
```

and a `tmake_file` line for it in the `x86_64-*-elf*` stanza of `gcc/config.gcc`.
GCC then picks `lib/gcc/x86_64-elf/14.2.0/no-red-zone/libgcc.a` automatically
whenever `-mno-red-zone` is on the command line. Whether the original install had this
variant is unknown; the build either way links with the flags the makefiles already pass.

## Back it up

`~/opt/cross` is about 300 MB and takes 15 minutes to rebuild, so it is in the nightly
home backup's include list rather than excluded. Build dependencies on Ubuntu:
`build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo`.
