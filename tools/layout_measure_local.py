#!/usr/bin/env python3
"""Generate the non-positional companion to the unchanged 5a timing page."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
args = parser.parse_args()
source = Path(__file__).parent / "pages/layout-measure.html"
page = source.read_text()
old = "#rows p:nth-child(even){background:#e6f0f6}"
assert page.count(old) == 1, "review the timing page's positional rule"
# Both versions retain the same DOM and script. Only the row paint rule
# changes, so removal/insertion no longer shifts selector matches elsewhere.
page = page.replace(old, "#rows p{background:#e6f0f6}")
page = page.replace("A Matter of Time", "A Local Matter of Time")
page = page.replace("yonder-layout-5a-v1", "yonder-layout-local-v1")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(page)
