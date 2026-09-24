# Private numeric conversion dependencies

libpage uses these implementations privately; it adds no libc ABI and no
kernel dependency. `number.c` owns HTML lexical rules, finite-value checks,
and fixed/exponential notation thresholds. `range.c` owns range arithmetic.

## musl decimal conversion

Source: https://git.musl-libc.org/cgit/musl/
Commit: `5e9972eaef08ccf55dabe254ac829a30329793d3`.
Original files are retained in `musl/`: `src/internal/floatscan.c`,
`src/math/scalbnl.c`, `src/math/fmodl.c`, and root `COPYRIGHT`.
License: MIT and the file-level notices retained in these sources.

`../number_decimal.inc` keeps the decimal branch of floatscan. The adapter
replaces FILE/shgetc with a bounded string cursor and errno with per-call
error state. It omits hexadecimal, infinity and NaN syntax, which HTML does
not accept. `../number_scale.inc` keeps x86-64 extended-precision scaling;
`../number_mod.inc` keeps the corresponding remainder arithmetic. Symbols
are private; explicit casts and parentheses accommodate the project's strict
warning flags. The arithmetic does not allocate. `number.c` checks binary64,
80-bit long double and little-endian ABI assumptions at compile time.

## Ryū shortest formatting

Source: https://github.com/ulfjack/ryu
Commit: `4c0618b0e44f7ef027ebae05d2cc7812048f7c8f`.
`ryu/ryu/` contains unmodified d2s source and its referenced headers.
Upstream offers Apache 2.0 or Boost 1.0; os64 uses the Boost 1.0 option.
Both upstream license files are retained. `ryu/compat/` provides the small
freestanding header surface for the guest build; host tests use libc headers.
Only `d2s_buffered_n` is called. Its finite input precondition is enforced by
libpage. Upstream's unused allocating wrapper uses os64_malloc in the guest.

## Reproduction and distribution

`SHA256SUMS` covers the retained upstream originals. From this directory run
`sha256sum -c SHA256SUMS`. The root `license/libpage-numeric-LICENSE` contains
the musl and Boost notices and is installed as `/etc/licenses/libpage-numeric.txt`
on both ext2 and FAT images. The host suite checks random binary64 round
trips, compares long decimal parsing against libc, and checks range arithmetic
against the independent Python Decimal oracle. `pagetest` exercises the
conversion and state paths on os64, yielding between numeric cases.
