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
also uses the shipped default.

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
