# Publishing Porter packages

Porter publishes complete installable file sets from a build checkout. Each
publication can name one system target and any number of independently managed
collections. The guest preview and install rules are described in the design.
[Porter design](INSTALLER.md)

## Publish a build

```sh
make package
make package NAME="Audio preview"
make package BASELINE=1 NAME="November userland baseline"
```

Run the publisher from the Linux or WSL checkout with Python 3.10 or later.
The default repository is a sibling of the primary checkout, named
`<checkout>-porter`. Set `PORTER_REPO=/absolute/path` to share another repository
outside the checkout with Windows Python. The publishing filesystem must
support advisory locks, atomic same-directory rename and directory sync.
Build the normal images through `make` as usual. Publication starts through
the explicit `package` target.

With no command-line `NAME`, the label uses the branch and the local date and
time with its UTC offset. `BASELINE=1` marks the publication as a candidate
for baseline adoption. The label and baseline flag live in the publication
record. Manifests and objects are addressed by their contents, and each
publication receives its own ID even when the label and contents repeat.

The system scope uses Make's staged apps, aliases, libraries, test fixtures
and full kernel, plus the boot modules, system config, licenses, trust roots,
fonts, frames, pages and icons selected by the image build. It declares the
exact kernel ELF SHA-256 as its kernel contract. The boot menu, EFI loader,
filesystem test scaffolding, partition markers and user data have their own
lifecycles.

## Add recursive collections

The default build-side source file is `porter-sources.conf` in the checkout.
`PORTER_SOURCES` selects an explicit file, and `PORTER_SOURCES_LOCAL` adds a
local overlay. An explicitly named file and the roots of selected collections
must be readable. Relative directory names start at the publishing checkout.

```ini
[sources]
fonts += extras/system fonts

[sources recordings]
sounds += /mnt/media/Recordings

[sources themes]
themepacks += local/Theme Packs
```

```sh
make package SCOPES=recordings NAME="Field recordings"
make package SCOPES=system,themes NAME="Desktop preview"
```

A source root contributes its contents, retaining relative subdirectories and
empty directories. A registered nested root owns its subtree. Its parent skips
that subtree even when the nested root's scope is outside the publication.
Files and directories with equal lot, relative path, kind and content
coalesce. Other definitions at that key report both inputs.
Links, Windows reparse points and special files are excluded from collection.
An empty scanned root still declares its lot. The source paths and overlays
remain on the publishing machine.

Each scope allows up to 16,384 entries. Names in `LOT/PATH` must fit the Os64
ustar encoding and its 255-byte path limit. File sizes use the ustar limit of
8,589,934,591 bytes. These are transport limits. The guest's destination
filesystem can impose further limits.

## Serve from Windows

Run the updated `tools/os64serve.py` next to `tools/porter_store.py` using
Windows Python. Add `--porter-repo` to the existing valet invocation and
supply the Windows-visible path to the publication repository.

```powershell
python.exe '\\wsl$\<distro>\home\<you>\src\os64\tools\os64serve.py' --porter-repo '\\wsl$\<distro>\home\<you>\src\os64-porter' 'C:\Os64\bin'
```

A repository-only invocation serves Porter. Existing directory arguments
continue to serve legacy `GET` and `LIST` on the same port. Point the server at
the repository itself, so it can see later atomic publications without a
restart. [Legacy server setup](../completed/OS64GET.md#running-os64serve)

## Porter v1 wire format

Each request uses its own TCP connection and starts with an ASCII line ending
in LF. These operations use the existing trusted-LAN listener.

| Request | Successful response |
| --- | --- |
| `CATALOG` | `OK\n`, then one canonical publication JSON object per line, then `.\n` |
| `MANIFEST ID` | `OK LENGTH\n`, then exactly `LENGTH` canonical JSON bytes |
| `ARCHIVE ID` | `OK LENGTH\n`, then exactly `LENGTH` bytes of a standalone tar.gz |
| `ARCHIVE ID LENGTH` followed by a JSON array | Same archive framing for the selected manifest entries |

An `ID` is a manifest's lowercase hexadecimal SHA-256. Canonical JSON uses ASCII
escapes, sorted object keys, compact separators and zero trailing bytes. A
selection is a compact JSON array of strictly increasing zero-based entry
indices, at most 128 KiB, with exactly the declared byte length. An empty
selection yields an empty archive. The server sends selected entries in
manifest order. Include a directory's index when it is part of the selection.

An unavailable target or malformed request returns `NO reason\n`. A stream
failure after the success header ends the connection without completing the
response. A receiver checks the declared length or catalog terminator, then
validates the archive and manifest before using the bytes.

A manifest records the version, scope, kernel contract, selected lots and
entries sorted by lot and relative path. A file entry has its uncompressed
length and SHA-256. A directory entry has its kind and path. Catalog records
map names and source provenance to manifest IDs. The catalog streams metadata
directly from the completed publication records.

The repository stores one gzip member per distinct file, containing its bytes
and zero padding to a 512-byte boundary. Serving a tar.gz combines normalized
ustar header members, stored data members and a two-block terminator. The
server verifies members while streaming in bounded chunks. The repository
keeps the objects and manifests reachable from its catalog, so completed
publications remain available when their input files or checkouts are removed.
