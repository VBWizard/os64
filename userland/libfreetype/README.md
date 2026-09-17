# libfreetype — os64's font engine

`/lib/libfreetype.so` turns outline fonts into grayscale coverage masks. It
is FreeType 2.14.3, pinned and trimmed, behind an os64-owned interface that
lets no FreeType type reach a consumer.

**It is a LEAF.** No `DT_NEEDED` at all — not even libos64 — so libos64 can
consume it later without the two importing each other. That is why the port
carries its own C runtime, and why `--no-undefined` on the link is what turns
the property from an intention into a build failure.

## What is where

| | |
|---|---|
| `upstream/` | the pinned FreeType sources, imported whole per module |
| `patches/` | upstream fixes backported onto the pin, applied in order |
| `manifest.json` | provenance plus a digest for every imported file |
| `port/backend.c` | the adapter: the whole of what this library exports |
| `port/os64_ft*.h`, `port/ft2build.h` | which FreeType this is — options, modules, and the C library it may call |
| `port/runtime.c`, `port/nonlocal.S` | the freestanding C runtime a leaf has to bring |
| `port/ftsystem.c` | the refusal that replaces upstream's platform layer |
| `fixtures/` | four licensed faces the tests measure against |
| `UPSTREAM_REVIEW.md` | the source audit: the pin, the fixes taken and left, the configuration's consequences |

The interface is F0's, declared in
`userland/libos64/include/os64/font_backend.h` and specified in
`FONT_CONTRACTS.md`. Consumers call `os64_freetype_backend_v1()` and check
`revision` and `struct_size` before using the table; it is the only symbol
this library exports.

## Building and checking it

```sh
make -C userland                                   # the library, with everything else
x86_64-elf-readelf -dW  userland/bin/libfreetype.so # expect NO DT_NEEDED
x86_64-elf-nm -D -u     userland/bin/libfreetype.so # expect nothing
x86_64-elf-nm -D --defined-only userland/bin/libfreetype.so  # expect one symbol
x86_64-elf-readelf -lW  userland/bin/libfreetype.so # expect R / R+E / RW, never RWX
x86_64-elf-readelf -rW  userland/bin/libfreetype.so # expect R_X86_64_RELATIVE only
x86_64-elf-size         userland/bin/libfreetype.so
```

The four expectations above are the library's contract with the loader, and
each fails loudly rather than quietly: a `DT_NEEDED` means the leaf property
is gone, an undefined symbol means `--no-undefined` was bypassed, a second
exported symbol means the version script was widened, and a writable
executable segment means `-z separate-code` stopped working.

## Testing it

**On the host, under the sanitizers** — where malformed fonts are hunted,
because a sanitizer can watch a parser walk off a table and QEMU cannot:

```sh
tools/test_freetype_host.py            # ASan + UBSan at -O2, the level that ships
tools/test_freetype_host.py --plain    # no sanitizers, for a fast pass
tools/test_freetype_host.py -O0        # the same tests at another level
python3 tools/test_freetype_target_runtime.py --mutation # target runtime + flag policy
```

It compiles the production sources with the host compiler and the same
configuration headers, so what is under test is what ships. It walks
truncation at ten lengths per font, a byte-by-byte character-map corruption
sweep, and allocation denial across the exercised creation/open/render paths,
requiring recovery with no leak. Metadata, container and Unicode-map fixtures
cover the adapter boundaries. A link wrapper reverses one rendered bitmap's
storage to check negative pitch; other renders call the rasterizer unchanged.
The target-runtime runner and guest also exercise the production private memory
aliases that host sanitizers replace. See
[the implementation review](../../docs/fonts/F1-IMPLEMENTATION-REVIEW.md) for
results and the observed effect of removing the loop-pattern flag. It ends
with a cost table: what one open face holds, per font and size.

**In the guest**, where the question is whether it works at ring 3 on os64's
allocator against real files:

```sh
make && make run                       # then, in the shell:
fonttest                               # or: fonttest /some/other/font/directory
```

`/tests/fonttest` asserts, prints a glyph's coverage mask as characters so a
text boot still shows something a person can judge, and — on a GUI boot —
opens a window of antialiased text at four sizes in all four faces. Without a
screen that last step skips; without font fixtures the whole thing skips,
because "I cannot run here" is not "I failed".

## Re-deriving the fixture expectations

```sh
tools/font_kern_report.py --fixtures        # the table in fixtures/FIXTURES.md
tools/font_kern_report.py --pairs AV,To --ppem 16,32 <font>…
```

The kerning numbers the tests assert against are not the engine's own output —
they come from this reader, which parses `kern` and `GPOS` straight out of the
font and shares no code with FreeType or with `port/backend.c`. It also
reports WHICH table each face's kerning came from, which is how the fixture
set was chosen.

## Re-deriving the import

```sh
tools/import_freetype.py --verify      # does upstream/ still match manifest.json?
tools/import_freetype.py --import      # rebuild it from the pinned release archive
```

`--verify` is the cheap one and the one to run when something looks wrong in
`upstream/`: it re-hashes all 334 files and every patch against the manifest.

## Changing it

- **Options and module selection belong in `port/`**, never in an edit inside
  `upstream/`. A switch changed in `port/os64_ftoption.h` survives a version
  bump as a diff anybody can read; an edit inside `upstream/` becomes a patch
  to rebase forever.
- **A genuine upstream fix goes in `patches/`**, as upstream's own `.patch`
  with its commit SHA in the header, and gets a row in UPSTREAM_REVIEW.md
  saying why it is there.
- **Adding a module means adding its whole source directory** to
  `tools/import_freetype.py` as well as its amalgamation to `sources.mk`. The
  two lists answer to each other; a mismatch is a link error rather than a
  silent half-build.
- **The fixtures' pinned kerning values belong to those exact files.** A font
  version bump invalidates them; re-derive with
  `tools/font_kern_report.py --fixtures` rather than assuming they carried
  over.
