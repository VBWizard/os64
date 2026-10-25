#!/usr/bin/env python3
"""Package the Yonder XP Appearance, frame and icons for extraction into /home."""
import gzip
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'output/yonder-xp'
ICONS = ROOT / 'assets/icons/yonder-xp'

INSTRUCTIONS = '''Yonder XP appearance pack

Copy yonder-xp.tar.gz to /home on Os64, then run:

    cd /home
    gunzip -c /home/yonder-xp.tar.gz | tar -x -f -

This installs themes/Yonder-XP.theme, frames/Yonder-XP.frame and
icons/yonder-xp/. Extracting the pack does not apply its settings.
Re-extraction replaces those named files, so save personal edits under
a different name first. If your configuration target is not /home, put
the themes and frames directories in that target instead.

In Appearance, select Yonder-XP in the saved collection, load it and Apply.
In Frame Studio, select Yonder-XP in Saved, load it and Apply.
Use each app's startup control if you want the look to persist after reboot.

Edit the set entry in /home/icons.conf (or your effective icons.conf):

    set = /home/icons/yonder-xp

Start new instances of applications that use icon sets to see the change.
To restore the shipped icons, set it back to /etc/icons/default.
Icons are separate from Appearance and Frame Studio; there is no icon GUI
chooser or live icon switch. This pack contains all 26 supported semantic
icons in 16, 24, 32, 48 and 64 pixel sizes.

The frame uses the current rectangular window painter and embeds its title
font. Curved frames are separate work. The Appearance does not choose
application font roles; wallpaper and desktop layout are separate.

Rebuild, host archive and transfer workflow: docs/APPEARANCE_PACKS.md in
the source repository.

Icon artwork: OpenAI built-in image generation, exported from committed
masters. The icon directory includes its license and provenance hashes;
the repository's assets/icons/sources/yonder-xp/generation.json records
the prompts. No Microsoft artwork is included.
'''


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / 'tools/build_icon_assets.py'),
                    '--set', 'yonder-xp', '--name', 'Yonder XP'], check=True)
    expected = json.loads((ROOT / 'assets/icons/default/provenance.json').read_text())
    actual = json.loads((ICONS / 'provenance.json').read_text())
    if {x['key'] for x in actual['icons']} != {x['key'] for x in expected['icons']}:
        raise SystemExit('Yonder XP must cover the shipped semantic icon keys.')
    for icon in actual['icons']:
        if {v['size'] for v in icon['variants']} != {16, 24, 32, 48, 64}:
            raise SystemExit('Incomplete icon sizes: '+icon['key'])
        for variant in icon['variants']:
            data = (ICONS / variant['file']).read_bytes()
            if hashlib.sha256(data).hexdigest() != variant['sha256']:
                raise SystemExit('Icon hash mismatch: '+variant['file'])
    members = {
        'themes/Yonder-XP.theme': (ROOT / 'appearances/Yonder-XP.theme').read_bytes(),
        'frames/Yonder-XP.frame': (ROOT / 'frames/Yonder-XP.frame').read_bytes(),
        'yonder-xp-README.txt': INSTRUCTIONS.encode(),
    }
    for source in sorted(ICONS.iterdir()):
        if source.is_file():
            members['icons/yonder-xp/'+source.name] = source.read_bytes()
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for directory in ['themes', 'frames', 'icons', 'icons/yonder-xp']:
            info = tarfile.TarInfo(directory+'/')
            info.type = tarfile.DIRTYPE
            info.mode = 0o755
            tar.addfile(info)
        for name, data in sorted(members.items()):
            info = tarfile.TarInfo(name)
            info.mode = 0o644
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    archive = OUTPUT / 'yonder-xp.tar.gz'
    with archive.open('wb') as out:
        with gzip.GzipFile(filename='', fileobj=out, mode='wb', mtime=0) as gz:
            gz.write(payload.getvalue())
    (OUTPUT / 'INSTALL.txt').write_text(INSTRUCTIONS)
    (OUTPUT / 'icons-contact-sheet.png').write_bytes(
        (ROOT / 'assets/icons/yonder-xp-contact-sheet.png').read_bytes())
    print(f'{archive}: {len(members)} files, {archive.stat().st_size} bytes')


if __name__ == '__main__':
    main()
