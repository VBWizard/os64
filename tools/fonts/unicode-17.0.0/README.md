# W1 reference inputs

UnicodeData.txt is the unchanged Unicode 17.0.0 data file from
https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt.
LICENSE.txt was retrieved from https://www.unicode.org/license.txt alongside it.
Both SHA256 digests are pinned in `../generate_w1.py`. The generator rejects
other bytes and does not use the host Python Unicode database. It extracts the
finite composition/Latin-letter rules in FONT_CONTRACTS.md and the box-drawing
stroke descriptions. `python3 tools/fonts/generate_w1.py --check` reproduces the
checked-in header; the source data and license are retained for offline review.
