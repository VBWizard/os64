#!/usr/bin/env python3
"""import_freetype.py — pin, fetch, verify and import the FreeType sources.

WHY A SCRIPT AND NOT A README PARAGRAPH
An imported third-party tree is only trustworthy if someone can re-derive it.
This file IS the provenance record: the release, the two mirrors it can be
fetched from, the archive digest, the modules that were taken, the patches
applied on top, and a per-file digest of everything that landed in
`userland/libfreetype/upstream/`. `--verify` re-hashes the working tree
against that record, so drift — an accidental edit, a half-applied patch —
is a command away rather than an act of faith.

  tools/import_freetype.py --verify    # is the tree still what the manifest says?
  tools/import_freetype.py --import    # re-derive the tree from the release archive

The archive is NOT kept in the repository: the imported files are, and their
digests are what the manifest checks. `--import` downloads to a scratch
directory, verifies the archive digest first, and refuses to proceed if it
does not match.

PATCHES are applied in sorted order with `patch -p1` from inside upstream/,
which works because the import preserves the archive's `src/` and `include/`
layout — the same prefix upstream's own commits carry. Each patch file names
the upstream commit it came from; UPSTREAM_REVIEW.md carries the argument for
why each one is here and why the ones left out were left out.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

# ---------------------------------------------------------------- the pin ---

RELEASE = "2.14.3"

# The RELEASE ARCHIVE, not a GitHub-generated tarball. GitHub regenerates
# those on demand and has changed their bytes before; the release archive is
# a fixed artifact, signed, and mirrored. Both mirrors below served identical
# bytes at import time (recorded in UPSTREAM_REVIEW.md).
ARCHIVE_NAME = f"freetype-{RELEASE}.tar.xz"
ARCHIVE_URLS = [
    f"https://download.savannah.gnu.org/releases/freetype/{ARCHIVE_NAME}",
    f"https://downloads.sourceforge.net/project/freetype/freetype2/{RELEASE}/{ARCHIVE_NAME}",
]
ARCHIVE_SHA256 = "36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f"

# THE TAG OBJECT AND THE COMMIT ARE DIFFERENT SHAS, and the difference is a
# trap. `VER-2-14-3` is an ANNOTATED tag, so GitHub's generated tarball is
# named after the TAG OBJECT (c740f0f...) — which is what an earlier probe
# recorded as "the commit". The commit the tag points at is 0a0221a...; that
# is the revision this import claims.
TAG = "VER-2-14-3"
TAG_OBJECT = "c740f0fda4274d6ffd2e5b64a25b06ef69803a07"
COMMIT = "0a0221a1347e2f1e07c395263540026e9a0aa7c7"

# The modules whose sources are imported. Each is taken WHOLE, because
# upstream's supported single-object build compiles one amalgamation per
# module (`src/cff/cff.c` includes every other file in `src/cff/`), and the
# `#ifdef`s inside those files — not a hand-picked file list — are what
# decides which code survives the configuration in port/ftoption_os64.h.
MODULES = [
    "autofit",    # the hinter: os64 renders with the autofitter
    "base",       # core objects, calculation, outlines, streams
    "cff",        # OpenType/CFF outlines
    "psaux",      # CFF charstring interpreter (required by cff)
    "pshinter",   # PostScript hinter (required by cff)
    "psnames",    # glyph-name tables (required by cff)
    "sfnt",       # the SFNT container shared by TrueType and OpenType
    "smooth",     # the grayscale rasterizer
    "truetype",   # TrueType outlines
]

# Kept for the record rather than for the build: the licence os64 exercises,
# the upstream change log the audit cites, the customization documents that
# define what the port's configuration headers are allowed to do, and the
# module manifest whose contents the MODULES list above answers to.
DOC_FILES = [
    "LICENSE.TXT",
    "README",
    "docs/CHANGES",
    "docs/CUSTOMIZE",
    "docs/FTL.TXT",
    "docs/INSTALL.ANY",
    "modules.cfg",
]

# `include/dlg/` and `src/dlg/` are the bundled dlg logging library, reachable
# only through FT_DEBUG_LOGGING. os64 does not build it, so it is not imported
# — and its absence is the one difference between this import and the tagged
# git tree, which does not carry dlg at all (it is a submodule there).
EXCLUDE_INCLUDE_DIRS = {"dlg"}

REPO = Path(__file__).resolve().parent.parent
LIB = REPO / "userland" / "libfreetype"
UPSTREAM = LIB / "upstream"
PATCHES = LIB / "patches"
MANIFEST = LIB / "manifest.json"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tree_digests(root):
    """Every regular file under root, keyed by its path relative to root."""
    out = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            out[str(path.relative_to(root))] = sha256_file(path)
    return out


def fetch_archive(dest):
    """Download the release archive and check its digest before unpacking it."""
    last = None
    for url in ARCHIVE_URLS:
        try:
            print(f"  fetching {url}")
            with urllib.request.urlopen(url, timeout=120) as r, open(dest, "wb") as f:
                shutil.copyfileobj(r, f)
            break
        except Exception as exc:                     # noqa: BLE001 - report and try the mirror
            last = exc
            print(f"  failed: {exc}")
    else:
        raise SystemExit(f"could not fetch {ARCHIVE_NAME}: {last}")

    got = sha256_file(dest)
    if got != ARCHIVE_SHA256:
        raise SystemExit(
            f"archive digest mismatch\n  expected {ARCHIVE_SHA256}\n  got      {got}"
        )
    print(f"  archive sha256 {got} OK")


def copy_tree(src, dst, skip_dirs=()):
    for path in sorted(src.rglob("*")):
        rel = path.relative_to(src)
        if rel.parts and rel.parts[0] in skip_dirs:
            continue
        target = dst / rel
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif path.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def apply_patches():
    """Apply patches/*.patch in sorted order, from inside upstream/."""
    applied = []
    for patch in sorted(PATCHES.glob("*.patch")):
        subject = ""
        commit = ""
        for line in patch.read_text(errors="replace").splitlines():
            if line.startswith("From ") and not commit:
                commit = line.split()[1]
            if line.startswith("Subject:") and not subject:
                subject = line[len("Subject:"):].strip()
            if commit and subject:
                break
        print(f"  applying {patch.name}")
        subprocess.run(
            ["patch", "-p1", "--no-backup-if-mismatch", "-i", str(patch)],
            cwd=UPSTREAM, check=True, stdout=subprocess.DEVNULL,
        )
        applied.append({
            "file": patch.name,
            "upstream_commit": commit,
            "subject": subject,
            "sha256": sha256_file(patch),
        })
    return applied


def do_import():
    with tempfile.TemporaryDirectory(prefix="ft-import-") as tmp:
        tmp = Path(tmp)
        archive = tmp / ARCHIVE_NAME
        fetch_archive(archive)

        print("  unpacking")
        with tarfile.open(archive) as tar:
            tar.extractall(tmp)                      # noqa: S202 - digest-verified archive
        src = tmp / f"freetype-{RELEASE}"
        if not src.is_dir():
            raise SystemExit(f"archive did not contain freetype-{RELEASE}/")

        if UPSTREAM.exists():
            shutil.rmtree(UPSTREAM)
        UPSTREAM.mkdir(parents=True)

        print("  importing include/")
        copy_tree(src / "include", UPSTREAM / "include", EXCLUDE_INCLUDE_DIRS)
        for module in MODULES:
            print(f"  importing src/{module}/")
            copy_tree(src / "src" / module, UPSTREAM / "src" / module)
        for doc in DOC_FILES:
            target = UPSTREAM / doc
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src / doc, target)

    applied = apply_patches()

    manifest = {
        "release": RELEASE,
        "tag": TAG,
        "tag_object": TAG_OBJECT,
        "commit": COMMIT,
        "archive": ARCHIVE_NAME,
        "archive_sha256": ARCHIVE_SHA256,
        "archive_urls": ARCHIVE_URLS,
        "modules": MODULES,
        "patches": applied,
        "files": tree_digests(UPSTREAM),
    }
    MANIFEST.write_text(json.dumps(manifest, indent=2, sort_keys=False) + "\n")
    print(f"  {len(manifest['files'])} files imported, {len(applied)} patches applied")
    print(f"  manifest written to {MANIFEST.relative_to(REPO)}")


def do_verify():
    if not MANIFEST.exists():
        raise SystemExit(f"{MANIFEST} is missing; run --import")
    manifest = json.loads(MANIFEST.read_text())

    for patch in manifest["patches"]:
        path = PATCHES / patch["file"]
        if not path.exists():
            raise SystemExit(f"patch {patch['file']} named by the manifest is missing")
        got = sha256_file(path)
        if got != patch["sha256"]:
            raise SystemExit(f"patch {patch['file']} changed: {got} != {patch['sha256']}")

    have = tree_digests(UPSTREAM)
    want = manifest["files"]
    missing = sorted(set(want) - set(have))
    extra = sorted(set(have) - set(want))
    changed = sorted(p for p in set(have) & set(want) if have[p] != want[p])

    for path in missing:
        print(f"  MISSING {path}")
    for path in extra:
        print(f"  EXTRA   {path}")
    for path in changed:
        print(f"  CHANGED {path}")
    if missing or extra or changed:
        raise SystemExit(
            f"upstream tree does not match the manifest "
            f"({len(missing)} missing, {len(extra)} extra, {len(changed)} changed)"
        )
    print(
        f"  {len(have)} files match the manifest; "
        f"{len(manifest['patches'])} patches unchanged"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--import", dest="do_import", action="store_true",
                    help="re-derive upstream/ from the pinned release archive")
    ap.add_argument("--verify", action="store_true",
                    help="check upstream/ and patches/ against manifest.json")
    args = ap.parse_args()
    if not (args.do_import or args.verify):
        ap.error("pass --import or --verify")
    if args.do_import:
        print(f"importing FreeType {RELEASE} ({TAG}, commit {COMMIT[:12]})")
        do_import()
    if args.verify:
        print(f"verifying FreeType {RELEASE} import")
        do_verify()


if __name__ == "__main__":
    main()
    sys.exit(0)
