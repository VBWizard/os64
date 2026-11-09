#!/usr/bin/env python3
"""Export reproducible PNG sizes, provenance, contact sheet and install archive.

ImageMagick performs size export from the committed transparent masters.
Pillow assembles the validation contact sheet without resizing its icon samples.
No runtime resampling is needed by Os64.
"""
import argparse
import gzip
import hashlib
import io
import json
import re
from datetime import date
from pathlib import Path
import shutil
import subprocess
import tarfile
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets/icons'
SOURCES = ASSETS / 'sources/default'
DEST = ASSETS / 'default'
SET_NAME = 'Roam Classic'
ARCHIVE_ROOT = 'roam-classic'
CONTACT_SHEET = ASSETS / 'contact-sheet.png'
SIZES = [16, 24, 32, 48, 64]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def convert(*args):
    subprocess.run(['convert', *map(str, args)], check=True)

def import_records(path):
    manifest_path = SOURCES / 'generation.json'
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {
        'origin': 'OpenAI built-in image generation',
        'date': date.today().isoformat(),
        'reference': 'Generated for this project without third-party reference images',
        'master_transform': 'ImageMagick Lanczos export to 256x256 RGBA; metadata stripped. Generated original SHA-256 retained per icon.',
        'icons': []}
    known = {x['key']: x for x in manifest['icons']}
    for entry in json.loads(path.read_text()):
        key = entry['key']
        source = Path(entry['path'])
        target = SOURCES / (key + '.png')
        convert(source, '-filter', 'Lanczos', '-resize', '256x256!', '-strip',
                '-define', 'png:exclude-chunks=date,time', 'PNG32:' + str(target))
        known[key] = dict(key=key, source=target.name,
            generated_sha256=digest(source), master_sha256=digest(target),
            prompt=entry['prompt'])
    manifest['icons'] = [known[k] for k in sorted(known)]
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')

def export():
    source = json.loads((SOURCES / 'generation.json').read_text())
    manifest = {'format': 1, 'name': SET_NAME,
        'origin': source['origin'], 'source_manifest': str((SOURCES / 'generation.json').relative_to(ROOT)),
        'license': 'Distributed under the repository MIT license to the extent rights exist; generated provenance is not a claim of exclusive copyright.',
        'sizes': SIZES, 'transform': 'Alpha trim, Lanczos fit within size minus 2 pixels, center on transparent square; RGBA8 PNG without ancillary metadata.',
        'icons': []}
    for entry in source['icons']:
        master = SOURCES / entry['source']
        assert digest(master) == entry['master_sha256'], master
        variants = []
        for size in SIZES:
            path = DEST / f'{entry["key"]}-{size}.png'
            convert(master, '-trim', '+repage', '-filter', 'Lanczos',
                '-resize', f'{size-2}x{size-2}', '-gravity', 'center',
                '-background', 'none', '-extent', f'{size}x{size}', '-strip',
                '-define', 'png:exclude-chunks=date,time', 'PNG32:' + str(path))
            im = Image.open(path)
            assert im.mode == 'RGBA' and im.size == (size, size)
            assert 0 in im.getchannel('A').getextrema()
            assert im.getchannel('A').getextrema()[1] == 255
            assert path.stat().st_size <= 65536
            variants.append(dict(size=size, file=path.name, sha256=digest(path)))
        manifest['icons'].append(dict(key=entry['key'], variants=variants))
    (DEST / 'provenance.json').write_text(json.dumps(manifest, indent=2) + '\n')
    shutil.copyfile(ROOT / 'LICENSE', DEST / 'LICENSE')
    contact_sheet([e['key'] for e in source['icons']])

def contact_sheet(keys):
    width, row = 1080, 76
    sheet = Image.new('RGB', (width, 92 + len(keys)*row), '#f5f1e8')
    draw = ImageDraw.Draw(sheet)
    face = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
    font = ImageFont.truetype(face, 13)
    title = ImageFont.truetype(face, 23)
    draw.text((18, 12), SET_NAME+' — shipped pixels at native size', font=title, fill='#172735')
    colors = ['#f5f1e8', '#202a36', '#386cab']
    for c, label in enumerate(['Light', 'Dark', 'Selected']):
        draw.text((210+c*280, 47), label+'   16 / 24 / 32 / 48 / 64', font=font, fill='#172735')
    for y, key in enumerate(keys):
        top = 84+y*row
        draw.text((12, top+28), key, font=font, fill='#172735')
        for c, bg in enumerate(colors):
            left = 200+c*280
            draw.rectangle((left, top, left+270, top+row-1), fill=bg)
            x = left+10
            for size in SIZES:
                im = Image.open(DEST/f'{key}-{size}.png')
                sheet.paste(im, (x, top+(row-size)//2), im)
                x += size+12
    sheet.save(CONTACT_SHEET)

def archive(path):
    # Deterministic archive: one relative root, regular files, no user paths.
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        directory = tarfile.TarInfo(ARCHIVE_ROOT+'/')
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o755
        tar.addfile(directory)
        for source in sorted(DEST.iterdir()):
            if not source.is_file(): continue
            info = tarfile.TarInfo(ARCHIVE_ROOT+'/'+source.name)
            info.size=source.stat().st_size
            info.mode=0o644
            tar.addfile(info, io.BytesIO(source.read_bytes()))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('wb') as out:
        with gzip.GzipFile(filename='', fileobj=out, mode='wb', mtime=0) as gz:
            gz.write(payload.getvalue())

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--set', default='default', help='Directory name under assets/icons')
    parser.add_argument('--name', help='Display name (default: Roam Classic for the default set)')
    parser.add_argument('--import-records', type=Path)
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', args.set):
        parser.error('--set must be a lowercase directory name with optional hyphens')
    SOURCES = ASSETS / 'sources' / args.set
    DEST = ASSETS / args.set
    SET_NAME = args.name or ('Roam Classic' if args.set == 'default' else args.set)
    ARCHIVE_ROOT = 'roam-classic' if args.set == 'default' else args.set
    CONTACT_SHEET = ASSETS / ('contact-sheet.png' if args.set == 'default' else args.set+'-contact-sheet.png')
    SOURCES.mkdir(parents=True, exist_ok=True); DEST.mkdir(parents=True, exist_ok=True)
    if args.import_records: import_records(args.import_records)
    export()
    if args.archive: archive(args.archive)
