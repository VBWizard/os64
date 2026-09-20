#!/usr/bin/env python3
"""Verify the selected F0 and F1 files against their recorded input snapshot."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--f1", type=Path, default=ROOT.parent / "freetype-backend")
    args = parser.parse_args()
    manifest = json.loads((ROOT / "docs/fonts/f0-review-inputs.json").read_text())
    failures = []
    count = 0
    for group, directory in (("f0", ROOT), ("f1", args.f1)):
        for relative, expected in manifest[group]["files"].items():
            count += 1
            try:
                actual = hashlib.sha256((directory / relative).read_bytes()).hexdigest()
            except OSError as error:
                failures.append(f"{group}/{relative}: {error}")
                continue
            if actual != expected:
                failures.append(f"{group}/{relative}: changed since review snapshot")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"PASS: {count} selected F0/F1 review inputs match {manifest['review_revision']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
