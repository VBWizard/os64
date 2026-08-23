#!/usr/bin/env python3
"""mkwall.py — turn any image into an os64 wallpaper (2026-08-23).

    tools/mkwall.py photo.jpg wall.ppm                # 1024x768, the P5's default
    tools/mkwall.py photo.png wall.ppm --size 1920x1080
    tools/mkwall.py photo.png wall.ppm --fit           # letterbox instead of crop

os64's compositor draws a wallpaper CENTERED and NEVER SCALED — the kernel
owns no scaler, and a wallpaper was not the reason to write one — so the
sizing happens here, on the host, where PIL already knows every format
anyone has ever saved a picture in. The output is a binary PPM (P6, 8-bit):
three header tokens and the bytes, the same format QEMU's `screendump`
writes, which is why it is the one the kernel reads.

Default is COVER (scale to fill, crop the overflow around the middle), the
thing a desktop wants; --fit keeps the whole picture and leaves the
desktop.conf color showing around it. Then:

    image = /home/wall.ppm        in /home/desktop.conf

and `os64get` it over (an `*.ppm = /home` row in os64get.conf, or whatever
directory the images are to live in).
"""
import argparse
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("mkwall.py needs Pillow: pip install pillow")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("dst", help="output .ppm")
    ap.add_argument("--size", default="1024x768", help="WxH of the screen (default 1024x768)")
    ap.add_argument("--fit", action="store_true",
                    help="shrink to fit entirely (letterbox) instead of cover-and-crop")
    args = ap.parse_args()

    w, h = (int(v) for v in args.size.lower().split("x"))
    im = Image.open(args.src).convert("RGB")
    sw, sh = im.size

    scale = (min if args.fit else max)(w / sw, h / sh)
    im = im.resize((max(1, round(sw * scale)), max(1, round(sh * scale))), Image.LANCZOS)
    if not args.fit:
        # crop the overflow around the middle — the same center the kernel uses
        left = (im.width - w) // 2
        top = (im.height - h) // 2
        im = im.crop((left, top, left + w, top + h))

    with open(args.dst, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % im.size)
        f.write(im.tobytes())
    print(f"{args.dst}: {im.width}x{im.height} P6, {im.width * im.height * 3 + 20} bytes")


if __name__ == "__main__":
    main()
