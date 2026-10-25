# Building and keeping appearance packs

An appearance pack combines a saved Appearance, a Frame Studio composition,
and an icon set. The repository holds the editable sources and prepared assets;
the packaging tools turn them into a distributable `.tar.gz`. Rebuilding a pack
uses the saved artwork, without calling an image generator again.

Keep versioned release archives in `~/os64_theme_packs` on the WSL host.
The P5 receives a copy for installation. The repository's ignored `output/`
directory is build staging, not the permanent archive. The WSL archive folder
is independent of the P5 and checkout; include it in host backups as well.

## What is kept in Git

The first pack is **Yonder XP**, inspired by blue Luna. Its parts are:

| Part | Source or prepared asset |
| --- | --- |
| Appearance colors and control treatment | `appearances/Yonder-XP.theme` |
| Editable frame recipe | `tools/frame_collection.c` |
| Prepared frame with embedded title font and finish tiles | `frames/Yonder-XP.frame` |
| Icon masters and generation prompts | `assets/icons/sources/yonder-xp/` |
| Installed PNG sizes, manifest, license and provenance | `assets/icons/yonder-xp/` |
| Icon exporter | `tools/build_icon_assets.py` |
| Combined pack builder and bundled instructions | `tools/build_yonder_xp_pack.py` |

The icon set has 26 semantic keys, exported at 16, 24, 32, 48 and 64 pixels.
The `.tar.gz` contains the prepared Appearance, frame, icon files and install
instructions. It does not contain the editable artwork masters or build tools.

## Rebuild the pack

Run from the root of the checkout containing the pack sources, in WSL/Linux.
The pack builder needs Python 3, Pillow, ImageMagick's `convert`, and
`/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf` for the contact-sheet labels.

```sh
python3 tools/build_yonder_xp_pack.py
```

This exports the committed icon masters, checks the icon-key coverage, sizes
and export hashes, then assembles these outputs:

```text
output/yonder-xp/yonder-xp.tar.gz
output/yonder-xp/INSTALL.txt
output/yonder-xp/icons-contact-sheet.png
```

It packages the existing `frames/Yonder-XP.frame`. If the frame recipe changed,
regenerate the prepared frames first. This step also needs a host C compiler;
the generator builds against the repository's pinned FreeType source and font.

```sh
python3 tools/generate_frame_collection.py
python3 tools/build_yonder_xp_pack.py
```

To check that the prepared frames match their recipes without replacing them:

```sh
python3 tools/generate_frame_collection.py --check --sanitize
```

Keep leak detection enabled. If a sandbox denies LeakSanitizer thread
attachment, run that check outside the sandbox with `ASAN_OPTIONS=detect_leaks=1`.

Rebuilding the same inputs with the same tools produces the same archive bytes.
Archive entry timestamps, ownership and gzip timestamps are fixed. The builder
does not require a kernel build, a running P5, or access to image generation.

## Keep a versioned release on the host

Commit the source and prepared-asset changes before recording a release.
Use a new version name for an updated pack; keep older releases available for
restoring a previous look. For a new release (v2 in this example), from the
checkout root:

```sh
mkdir -p ~/os64_theme_packs
test ! -e ~/os64_theme_packs/yonder-xp-v2.tar.gz && {
    cp output/yonder-xp/yonder-xp.tar.gz ~/os64_theme_packs/yonder-xp-v2.tar.gz &&
    cp output/yonder-xp/INSTALL.txt ~/os64_theme_packs/yonder-xp-v2-INSTALL.txt &&
    git rev-parse HEAD > ~/os64_theme_packs/yonder-xp-v2.commit &&
    (cd ~/os64_theme_packs && sha256sum yonder-xp-v2.tar.gz > yonder-xp-v2.tar.gz.sha256)
}
```

Choose an unused version before running the release commands. The checksum can
be checked later from the archive directory with:

```sh
sha256sum -c yonder-xp-v2.tar.gz.sha256
```

In Windows File Explorer, the folder is reachable through the WSL share:
`\\wsl.localhost\<distro>\home\<user>\os64_theme_packs`.

## Transfer through os64get

Add the host archive directory to the existing `os64serve.py` invocation.
For a dedicated server launched in WSL from the checkout root:

```sh
python3 tools/os64serve.py "$HOME/os64_theme_packs=themepacks"
```

Use the existing Windows/WSL network arrangement described in
[OS64GET.md](design/completed/OS64GET.md). When running the server from Windows,
supply the archive directory's WSL share path followed by `=themepacks` instead.
Only one server can bind a given address and port; add the directory to an
existing server rather than launching a second instance on its port.

On the P5, replace `HOST_PC` with the same server address used for other updates:

```sh
mkdir -p /var/themes
cd /var/themes
os64get HOST_PC yonder-xp-v2.tar.gz
```

The shipped `etc/os64get.conf` routes `@themepacks = /var/themes`. If a
customized `/home/os64get.conf` is in use, add that rule there too; the first
configuration file found wins. The server's `themepacks` label lets both single
file downloads and bulk updates route archives and installation instructions
to `/var/themes`. The existing `*.png` rule still routes the PNG contact sheet
to the image directory because suffix rules take precedence over ordinary lots.
An explicit destination of `/var/themes/yonder-xp-v2.tar.gz` can also be supplied
to `os64get` to override routing.

## Transfer through SSH

Os64's SSH exec channel carries binary input. It does not provide SFTP or scp.
From WSL Bash, set `OS64_HOST` to the P5 address and `SSH_KEY` to the enrolled
local identity. Create `/var/themes` on the P5 first (`mkdir -p /var/themes`),
then copy the archive with:

```sh
ssh -T -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes \
    -i "$SSH_KEY" "os64@$OS64_HOST" \
    'cat > /var/themes/yonder-xp-v2.tar.gz' \
    < "$HOME/os64_theme_packs/yonder-xp-v2.tar.gz"
```

The command transfers the file without extracting or applying it. It replaces
that destination if it exists. The host copy remains in the archive folder.
To verify the stored bytes, read them back and compare with the local archive:

```sh
ssh -T -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes \
    -i "$SSH_KEY" "os64@$OS64_HOST" \
    'cat /var/themes/yonder-xp-v2.tar.gz' > /tmp/yonder-xp-readback.tar.gz &&
    cmp "$HOME/os64_theme_packs/yonder-xp-v2.tar.gz" /tmp/yonder-xp-readback.tar.gz
```

## Install and select the look

Start in `/var/themes` on the P5, where os64get placed the archive. Change to
`/home` for extraction so the archive's relative paths become personal theme,
frame and icon directories, then return to `/var/themes`:

```sh
cd /home
gunzip -c /var/themes/yonder-xp-v2.tar.gz | tar -x -f -
cd /var/themes
```

Use the filename of the downloaded version if different. Os64's `tar` extracts
into its current directory and has no `-C` option.

Extraction installs `themes/Yonder-XP.theme`, `frames/Yonder-XP.frame`,
`icons/yonder-xp/` and `yonder-xp-README.txt`. Re-extraction replaces those named
files; save personal variations under another name. If the configuration target
is different from `/home`, place the themes and frames in that target instead.

1. In **Appearance**, load the saved **Yonder-XP** entry and Apply.
2. In **Frame Studio**, load **Yonder-XP** from Saved and Apply.
3. Edit the `set` entry in `/home/icons.conf`, or the effective `icons.conf`:

   ```text
   set = /home/icons/yonder-xp
   ```

4. Start new instances of applications that use icon sets. Icon selection has
   no GUI chooser or live switch. To restore the shipped icons, use
   `set = /etc/icons/default`.

Use the startup controls in Appearance and Frame Studio to keep their settings
after reboot. These three selections are independent; extracting a pack does
not select it. The frame embeds its title font; the Appearance snapshot does
not choose application font roles. Wallpaper and desktop layout are separate.
Yonder XP uses the rectangular frame painter; curved frames are separate work.
