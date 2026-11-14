#!/usr/bin/env python3
"""Stage a frozen recovery consumer cohort and observe its parent in QEMU.

Inputs are a bootable TESTRUN/NONET ISO and matching FAT disk image containing
/tests and /lib. Source images and existing output directories are preserved.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import time


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-image", type=Path, required=True)
    parser.add_argument("--boot-iso", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--partition-offset", type=int, default=1048576)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    if args.timeout <= 0 or args.partition_offset < 0:
        parser.error("timeout must be positive and partition offset nonnegative")
    root = Path(__file__).resolve().parents[1]
    subprocess.run(["make", "-C", str(root / "userland"), "-j8", "dom-recovery-test"], check=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cohort = output / "cohort"
    (cohort / "tests").mkdir(parents=True)
    (cohort / "lib").mkdir()
    for binary, name in [("recovery-test", "recoverytest"), ("recovery-parent", "testrun")]:
        shutil.copy2(root / "userland/obj/dom" / binary, cohort / "tests" / name)
    pending = list((cohort / "tests").iterdir())
    seen = set()
    while pending:
        binary = pending.pop()
        dynamic = subprocess.check_output(["x86_64-elf-readelf", "-d", str(binary)], text=True)
        for name in re.findall(r"Shared library: \[([^]]+)\]", dynamic):
            if Path(name).name != name:
                raise ValueError("library dependency must be a basename")
            if name in seen:
                continue
            seen.add(name)
            target = cohort / "lib" / name
            shutil.copy2(root / "userland/bin" / name, target)
            pending.append(target)
    files = sorted(p for p in cohort.rglob("*") if p.is_file())
    (cohort / "SHA256SUMS").write_text("".join(
        digest(p) + "  " + str(p.relative_to(cohort)) + "\n" for p in files))
    for source, name in [(args.base_image, "os64.img"), (args.boot_iso, "boot.iso")]:
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(source.resolve()),
                        str(output / name)], check=True)
    image = str(output / "os64.img") + "@@" + str(args.partition_offset)
    for source in files:
        subprocess.run(["mcopy", "-o", "-i", image, str(source),
                        "::/" + str(source.relative_to(cohort))], check=True)
    metadata = {
        "source_head": subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
        "working_diff_sha256": hashlib.sha256(subprocess.check_output(
            ["git", "-C", str(root), "diff", "--binary", "HEAD"])).hexdigest(),
        "source_files": {name: digest(root / name) for name in [
            "userland/libdom/include/dom/dom.h", "userland/libdom/recovery.inc",
            "userland/libdom/tests/recovery.c", "userland/libdom/tests/recovery_cases.h",
            "userland/libdom/tests/recovery_parent.c", "userland/libdom/library.mk",
            "userland/libjs/manifest.json", "tools/test_dom_recovery_guest.py"]},
        "boot_sha256": digest(output / "boot.iso"),
        "base_image_sha256": digest(args.base_image.resolve()),
        "files": len(files), "bytes": sum(p.stat().st_size for p in files),
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    command = ["qemu-system-x86_64", "-machine", "q35", "-m", "2G", "-smp", "2",
        "-display", "none", "-nic", "none", "-no-reboot", "-boot", "d", "-cdrom",
        str(output / "boot.iso"), "-drive", "file=" + str(output / "os64.img") + ",format=raw,if=none,id=nvme0",
        "-device", "nvme,drive=nvme0,serial=F16HOST", "-serial", "stdio", "-monitor", "none"]
    (output / "command.json").write_text(json.dumps(command) + "\n")
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    poll = selectors.DefaultSelector()
    poll.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + args.timeout
    used, tail, passed = 0, b"", False
    try:
        with (output / "serial.log").open("wb") as log:
            while time.monotonic() < deadline and used < 16 * 1024 * 1024:
                if not poll.select(1):
                    continue
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    break
                log.write(chunk)
                log.flush()
                used += len(chunk)
                tail = (tail + chunk)[-65536:]
                if b"F16 QEMU COMPLETE PASS" in tail or b"F16 QEMU COMPLETE FAIL" in tail:
                    passed = b"F16 QEMU COMPLETE PASS" in tail
                    break
    finally:
        poll.close()
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        process.stdout.close()
    print("F16 QEMU PASS" if passed else "F16 QEMU missing/failed parent verdict", flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
