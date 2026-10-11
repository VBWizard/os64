#!/usr/bin/env python3
"""Generate bounded WebP navigation stress pages outside the shipped demo.

Serve the output directory over HTTP to a disposable Os64 guest. Enable
scripts and diagnostics in that guest's yonder.conf, then run webpwatch
with the served start.html URL. Script-requested file: navigation is refused
by Yonder. close.html holds six images for a separate close-during-decode run.
No downloaded artwork is used.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from PIL import Image


def page(title, body, next_page=None, delay=0):
    script = ''
    if next_page:
        script = ('<script>setTimeout(function () { location.href = ' +
                  json.dumps(next_page) + '; }, ' + str(delay) + ');</script>')
    return ('<!doctype html><title>' + title + '</title>' +
            '<style>img { width: 128px; height: 128px; }</style>' +
            '<h1>' + title + '</h1>' + body + script + '\n')


def generate(out, fixture, rounds, active_ms, idle_ms, image_format='webp', size=4096):
    out.mkdir(parents=True, exist_ok=True)
    if fixture is None:
        fixture = out / ('source.' + image_format)
        ramp = Image.linear_gradient('L').resize((size, size))
        horizontal = ramp.transpose(Image.Transpose.TRANSPOSE)
        Image.merge('RGBA', (horizontal, ramp, horizontal, ramp)).save(
            fixture, image_format.upper(), quality=80, method=3, exact=True)
    data = fixture.read_bytes()
    assert len(data) <= 20 * 1024 * 1024
    with Image.open(fixture) as im:
        width, height = im.size
        assert width * height <= 16 * 1024 * 1024
        image_format = im.format.lower()
        assert image_format in ('webp', 'png')
        im.load()
    for i in range(6):
        shutil.copyfile(fixture, out / f'large-{i}.{image_format}')
    images = ''.join(f'<img src="large-{i}.{image_format}" alt="large {i}">' for i in range(6))
    (out / 'start.html').write_text(page('WebP stress: baseline',
        f'<p>Starting automatically. Six {width} by {height} images per round.</p>', 'load-00.html', idle_ms))
    for i in range(rounds):
        (out / f'load-{i:02}.html').write_text(page(f'WebP stress: load {i:02}',
            images, f'idle-{i:02}.html', active_ms))
        next_page = f'load-{i+1:02}.html' if i + 1 < rounds else 'done.html'
        (out / f'idle-{i:02}.html').write_text(page(f'WebP stress: idle {i:02}',
            '<p>Images released; waiting for cancelled workers to drain.</p>', next_page, idle_ms))
    (out / 'done.html').write_text(page('WebP stress: done',
        '<p>Navigation rounds finished. Leave open briefly to measure the idle heap, then close.</p>'))
    (out / 'close.html').write_text(page('WebP stress: close during decode', images))
    report = dict(rounds=rounds, active_ms=active_ms, idle_ms=idle_ms,
                  dimensions=[width, height], format=image_format, images_per_round=6, input_bytes=len(data),
                  fixture_sha256=hashlib.sha256(data).hexdigest())
    (out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fixture', type=Path)
    parser.add_argument('--format', choices=('webp', 'png'), default='webp',
                        help='generated image format; PNG supplies a decoder control')
    parser.add_argument('--size', type=int, choices=(256, 4096), default=4096,
                        help='square dimension; 256 supplies a transport/navigation control')
    parser.add_argument('--rounds', type=int, default=12)
    parser.add_argument('--active-ms', type=int, default=1000)
    parser.add_argument('--idle-ms', type=int, default=15000)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 30 or not 1 <= args.active_ms <= 10000 or not 1000 <= args.idle_ms <= 30000:
        parser.error('rounds 1..30, active-ms 1..10000, idle-ms 1000..30000 required')
    generate(args.output, args.fixture, args.rounds, args.active_ms, args.idle_ms, args.format, args.size)
