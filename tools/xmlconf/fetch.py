#!/usr/bin/env python3
"""Fetch the pinned upstream corpus, preserving its license notices."""
import hashlib
from pathlib import Path
import tarfile
import urllib.request

URL = 'https://www.w3.org/XML/Test/xmlts20130923.tar.gz'
SHA256 = '9b61db9f5dbffa545f4b8d78422167083a8568c59bd1129f94138f936cf6fc1f'
ROOT = Path(__file__).resolve().parents[2]
ARCHIVE = ROOT / '.vendor/xmlconf-download/xmlts20130923.tar.gz'


def verify():
    if hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() != SHA256:
        raise SystemExit('xmlconf archive digest mismatch')


def main():
    ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
    if not ARCHIVE.exists():
        with urllib.request.urlopen(URL, timeout=60) as response:
            data = response.read(8 * 1024 * 1024 + 1)
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise SystemExit('downloaded xmlconf archive digest mismatch')
        ARCHIVE.write_bytes(data)
    verify()
    with tarfile.open(ARCHIVE) as archive:
        for member in archive.getmembers():
            path = Path(member.name)
            if path.is_absolute() or '..' in path.parts or path.parts[0] != 'xmlconf' or not (member.isfile() or member.isdir()):
                raise SystemExit(f'unsafe archive member: {member.name}')
        archive.extractall(ROOT / '.vendor', filter='data')
    print(f'xmlconf verified and extracted: {SHA256}')


if __name__ == '__main__':
    main()
