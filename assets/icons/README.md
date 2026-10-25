# Icon sets

An icon set is an extracted folder. It contains `theme.conf` and PNG files
named `<key>-<size>.png`. The manifest uses the usual os64 config syntax:

```text
format = 1
name = Roam Classic
author = Os64 project (AI-generated artwork)
```

`format`, `name` and `author` are required. Asset paths are fixed by the key
and size; the manifest cannot redirect them. The shipped set in `default/` is
installed at `/etc/icons/default/`. Another set can supply any subset of its
icons; a missing icon comes from that shipped set. A malformed PNG remains an
error, so a broken replacement is visible during validation.

The supported sizes are **16, 24, 32, 48 and 64 px**, all square. The first
family contains these 26 keys:

| Group | Keys |
| --- | --- |
| Objects and places | `folder`, `file`, `drive`, `app`, `home` |
| Navigation | `go-back`, `go-forward`, `go-up`, `view-refresh` |
| Documents | `document-open`, `document-save`, `document-save-as`, `folder-new` |
| Editing | `edit-cut`, `edit-copy`, `edit-paste`, `edit-delete` |
| Information | `search`, `help`, `settings` |
| Views | `view-details`, `view-icons`, `view-preview`, `view-split`, `tab-new`, `window-close` |

## P5 updates through os64get

The shipped artwork is PNG, not Windows `.ico`. Add this source directory to
an existing `os64serve.py` command, using the E2 worktree's path:

```sh
assets/icons/default=icons
```

Create the destination on Os64 and add the following rule to the effective
`os64get.conf` (usually the customized `/home/os64get.conf`):

```sh
mkdir -p /etc/icons/default
```

```ini
@icons/* = /etc/icons/default
```

The `/*` is significant: this whole-lot rule keeps the PNGs, `theme.conf`,
provenance and license together despite the general `*.png` and `*.conf`
rules. An ordinary `@icons` loses to those suffix rules. Exact filename
rules still outrank whole-lot rules.

Bootstrap an older P5 client **before** the bulk refresh:

```sh
os64get HOST os64get /bin/os64get
os64get -a HOST
```

The second invocation uses the new client's whole-lot support. Without that
first invocation, the old client interprets `icons/*` as a literal lot label
and ignores the rule. Its running process does not adopt its own replacement.
The server needs to offer the E2 `os64get` binary as well as this asset folder.
A customized `/home/os64get.conf` wins over `/etc`; downloading a new system
configuration does not update the user's copy. Restart actiongallery after
installation. Serve one icon set per catalogue: the valet uses flat filenames,
so multiple sets would collide. Use the archive workflow for additional sets.

## Additional sets from archives

To install an archive that contains a top-level `roam-classic/` folder, place
the archive at `/home/roam-classic.tar.gz` in os64 and extract it:

```sh
mkdir -p /home/icons
cd /home/icons
gunzip -c /home/roam-classic.tar.gz | tar -x -f -
```

Select it in `icons.conf` on the normal config ladder. With the default
`/home:/etc` ladder, add this line to `/home/icons.conf`:

```text
set = /home/icons/roam-classic
```

If `/home/icons.conf` already exists, merge the setting into that file instead
of replacing its other contents. Start a new application instance to read the
selection. There is no GUI chooser or live switching. An absent or invalid
selected manifest uses `/etc/icons/default`; a missing icon in a valid set
also uses the shipped default. Detected manifest/config I/O or allocation
failures return an error instead of selecting another family; callers can retry
creation. An open failure followed by failed stat remains ambiguous under the
current filesystem ABI and follows the missing-manifest policy.

`sources/default/` holds the generated 256 px raster masters and
`generation.json`; `default/provenance.json` records their exports. The
130 installed PNGs are transparent 16/24/32/48/64 px exports from those masters,
not independently drawn variants. The original art was generated with OpenAI's
built-in image generation without external artwork. The files are distributed
under the repository's MIT license to the extent rights allow; no exclusive
rights to generated material are promised. Os64’s own `gunzip` and `tar` have been used to extract the set, and the
extracted files were compared byte for byte with these production assets.

To rebuild the PNG exports, provenance, contact sheet and distributable archive
on the host (requires ImageMagick, Pillow and DejaVu Sans):

```sh
python3 tools/build_icon_assets.py --archive output/icons/roam-classic.tar.gz
```

The runtime reads the extracted folder; it does not read compressed archives.

## Yonder XP

The [appearance-pack guide](../../docs/APPEARANCE_PACKS.md) covers the complete
rebuild, host archive, transfer and installation workflow.

The `yonder-xp/` family pairs glossy, colorful Luna-inspired artwork with
`appearances/Yonder-XP.theme` and `frames/Yonder-XP.frame`. Its original
generated masters and prompts are in `sources/yonder-xp/`; the installed
exports use the same 26 semantic keys and five sizes as the default family.

Rebuild the icons and their standalone archive with:

```sh
python3 tools/build_icon_assets.py --set yonder-xp --name 'Yonder XP' --archive output/icons/yonder-xp.tar.gz
```

Or build the combined Appearance, frame and icon pack:

```sh
python3 tools/build_yonder_xp_pack.py
```

The combined archive is `output/yonder-xp/yonder-xp.tar.gz`. Keep a versioned
host copy and serve it with the `themepacks` lot, which routes archives to
`/var/themes` on Os64. Follow `output/yonder-xp/INSTALL.txt` from that folder;
extraction runs in `/home` to create the personal collections without selecting
them. After extraction, the icon setting
is `set = /home/icons/yonder-xp`; open new application instances to read it.
The contact sheet is `output/yonder-xp/icons-contact-sheet.png`.
