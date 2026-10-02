#!/usr/bin/env python3
"""Check the actual JavaScript payload on the root and rescue image volumes."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
disk = root / "disk/os64.img"
ext2 = root / "disk/ext2_test.img"


def run(*args):
    return subprocess.run([str(arg) for arg in args], check=True,
                          capture_output=True, text=True).stdout


def sector(index, name):
    info = run("sgdisk", "-i", index, disk)
    if f"Partition name: '{name}'" not in info:
        raise SystemExit(f"FAIL: partition {index} is not {name}")
    return int(re.search(r"First sector: (\d+)", info).group(1)) * 512


def dependencies(path):
    return set(re.findall(r"\(NEEDED\).*\[(.*?)\]", run("x86_64-elf-readelf", "-dW", path)))


payload = {"/bin/js": root / "userland/bin/js"}
pending = ["/bin/js"]
needed = {}
while pending:
    guest = pending.pop()
    needed[guest] = dependencies(payload[guest])
    for name in sorted(needed[guest]):
        if Path(name).name != name:
            raise SystemExit(f"FAIL: {guest} has a non-library dependency {name}")
        target = "/lib/" + name
        if target not in payload:
            payload[target] = root / "userland/bin" / name
            pending.append(target)

payload["/etc/licenses/quickjs.txt"] = root / "userland/libjs/upstream/LICENSE"
manifest = json.loads((root / "userland/libjs/manifest.json").read_text())
notice = next(entry for entry in manifest["retained_files"] if entry["path"] == "upstream/LICENSE")
if hashlib.sha256(payload["/etc/licenses/quickjs.txt"].read_bytes()).hexdigest() != notice["sha256"]:
    raise SystemExit("FAIL: QuickJS notice differs from the pinned manifest")

for path, expected in [("/bin/js", {"libjs.so", "libos64.so"}),
                       ("/lib/libjs.so", {"libmath.so", "libos64.so"})]:
    found = needed.get(path, set())
    if found != expected:
        raise SystemExit(f"FAIL: {path} dependencies {found}, expected {expected}")

fat_offset = sector(1, "fat")
ext2_offset = sector(2, "ext2")
with tempfile.TemporaryDirectory(prefix="js-image-") as work:
    for volume, image in [("ext2 image", str(ext2)),
                          ("disk ext2 root", f"{disk}?offset={ext2_offset}"),
                          ("FAT rescue", f"{disk}@@{fat_offset}")]:
        for index, (guest, source) in enumerate(payload.items()):
            destination = Path(work) / str(index)
            destination.unlink(missing_ok=True)
            try:
                if volume == "FAT rescue":
                    run("mcopy", "-i", image, "::" + guest, destination)
                else:
                    run("debugfs", "-R", f"dump {guest} {destination}", image)
            except subprocess.CalledProcessError as error:
                raise SystemExit(f"FAIL: {volume} {guest} could not be read: {error.stderr.strip()}")
            if not destination.exists() or destination.read_bytes() != source.read_bytes():
                raise SystemExit(f"FAIL: {volume} {guest} is missing or differs from the build")
        print(f"PASS {volume}: runner, {len(needed) - 1} libraries and pinned QuickJS notice match the build")
print(f"PASS JavaScript image payload: {3 * len(payload)} byte comparisons and complete shared dependencies")
