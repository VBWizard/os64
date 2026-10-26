"""RGB reftest comparison and a deliberately bounded WPT metadata profile."""
from dataclasses import asdict, dataclass
from html.parser import HTMLParser
from pathlib import Path
import re
from urllib.parse import unquote, urljoin, urlsplit
import xml.etree.ElementTree as ET

from PIL import Image, ImageChops


class Unsupported(ValueError):
    """A fixture outside the runner's declared profile, not a test pass."""


def local_url(base, href):
    url = urljoin(base, href)
    parts = urlsplit(url)
    if parts.scheme or parts.netloc or '\\' in url or '\x00' in url:
        raise Unsupported(f'nonlocal reference: {href!r}')
    if not parts.path.startswith('/'):
        raise ValueError('fixture URL must be root-relative')
    return url


def file_for(root, url):
    path = (root / unquote(urlsplit(url).path).lstrip('/')).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError(f'missing or escaping fixture: {url}')
    return path


@dataclass(frozen=True)
class Fuzzy:
    difference: tuple = (0, 0)
    pixels: tuple = (0, 0)

    def equal(self, maximum, count):
        # The pinned WPT runner permits exact equality when either lower bound
        # is zero, including a positive lower bound on the other metric.
        if count == maximum == 0 and (self.difference[0] == 0 or self.pixels[0] == 0):
            return True
        return (self.difference[0] <= maximum <= self.difference[1]
                and self.pixels[0] <= count <= self.pixels[1])


def fuzzy_value(value):
    named, positional = {}, []
    parts = value.split(';')
    if len(parts) != 2:
        raise ValueError('fuzzy needs two ranges')
    for part in parts:
        name, sep, raw = part.partition('=')
        raw = raw.strip() if sep else name.strip()
        if not re.fullmatch(r'\d+(?:\s*-\s*\d+)?', raw):
            raise ValueError(f'invalid fuzzy range: {raw}')
        numbers = tuple(map(int, re.split(r'\s*-\s*', raw)))
        limits = numbers if len(numbers) == 2 else numbers * 2
        if limits[0] > limits[1]:
            raise ValueError('reversed fuzzy range')
        if sep:
            name = name.strip()
            if name not in ('maxDifference', 'totalPixels') or name in named:
                raise ValueError('unknown or duplicate fuzzy property')
            named[name] = limits
        else:
            positional.append(limits)
    values = [named[k] if k in named else positional.pop(0)
              for k in ('maxDifference', 'totalPixels')]
    if values[0][1] > 255:
        raise ValueError('channel difference exceeds 255')
    return Fuzzy(*values)


@dataclass
class Metadata:
    references: list
    fuzzy: dict

    def tolerance(self, ref):
        return self.fuzzy.get(ref, self.fuzzy.get(None, Fuzzy()))


class Tags(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.tags = []

    def handle_starttag(self, tag, attrs):
        self.tags.append((tag, dict(attrs)))


def metadata(root, url):
    path = file_for(root, url)
    text = path.read_text(encoding='utf-8')
    if path.suffix.lower() in ('.svg', '.xml', '.xhtml', '.xht'):
        if '<!DOCTYPE' in text.upper() or '<!ENTITY' in text.upper():
            raise Unsupported('DTD-bearing XML fixture')
        tree = ET.fromstring(text)
        tags = [(e.tag.removeprefix('{http://www.w3.org/1999/xhtml}'), e.attrib)
                for e in tree.iter()]
    else:
        parser = Tags()
        parser.feed(text)
        tags = parser.tags
    refs, fuzzy = [], {}
    for tag, attrs in tags:
        if tag == 'base':
            raise Unsupported('base element changes reference resolution')
        if tag == 'link' and attrs.get('rel') in ('match', 'mismatch'):
            if not attrs.get('href'):
                raise ValueError('reference has no href')
            refs.append((local_url(url, attrs['href']), attrs['rel']))
        if tag == 'meta' and attrs.get('name') == 'fuzzy':
            raw = attrs.get('content', '')
            key = None
            if ':' in raw:
                prefix, raw = raw.rsplit(':', 1)
                key = local_url(url, prefix.strip())
            if key in fuzzy:
                raise ValueError('duplicate fuzzy reference')
            fuzzy[key] = fuzzy_value(raw)
    for key in fuzzy:
        if key is not None and key not in [ref for ref, _ in refs]:
            raise ValueError(f'fuzzy metadata names no reference: {key}')
    return Metadata(refs, fuzzy)


def load(path):
    if path.stat().st_size > 64 * 1024 * 1024:
        raise ValueError('image file exceeds 64 MiB')
    with Image.open(path) as image:
        if image.width * image.height > 16 * 1024 * 1024:
            raise ValueError('image exceeds 16 million pixels')
        if getattr(image, 'n_frames', 1) != 1:
            raise Unsupported('animated image')
        rgba = image.convert('RGBA')
        if rgba.getchannel('A').getextrema() != (255, 255):
            raise ValueError('composite transparency over a declared background before comparison')
        return rgba.convert('RGB')


def compare(left, right, relation='match', fuzzy=Fuzzy(), diff_path=None):
    if relation not in ('match', 'mismatch'):
        raise ValueError('unknown reference relation')
    lhs, rhs = load(left), load(right)
    if lhs.size != rhs.size:
        raise ValueError(f'image size mismatch: {lhs.size} != {rhs.size}')
    diff = ImageChops.difference(lhs, rhs)
    # Count RGB differences directly: luminance conversion can round a one-unit
    # blue-channel change to zero and let a real difference disappear.
    count = maximum = absolute_sum = 0
    for pixel in diff.getdata():
        count += any(pixel)
        maximum = max(maximum, *pixel)
        absolute_sum += max(pixel)
    equal = fuzzy.equal(maximum, count)
    if diff_path:
        diff.save(diff_path)
    return {'pass': equal if relation == 'match' else not equal,
            'maxDifference': maximum, 'totalPixels': count,
            'sumMaxChannelDifference': absolute_sum,
            'relation': relation, 'fuzzy': asdict(fuzzy)}


def verdict(results):
    if not results:
        raise ValueError('a reftest needs at least one reference')
    matches = [r['pass'] for r in results if r['relation'] == 'match']
    mismatches = [r['pass'] for r in results if r['relation'] == 'mismatch']
    return (not matches or any(matches)) and all(mismatches)
