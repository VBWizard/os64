#!/usr/bin/env python3
"""Stage the private owner-metadata oracle and observe its parent in QEMU.

This consumer loads libos64, not libjs/QuickJS; it does not prove a runtime
incarnation issuer, negotiated profile, lifecycle or reclamation integration.

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
    for source in (args.base_image, args.boot_iso):
        if not source.is_file():
            parser.error("source image does not exist: " + str(source))
    root = Path(__file__).resolve().parents[1]
    subprocess.run(["make", "-C", str(root / "userland"), "-j8", "js-owner-tickets-test"], check=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cohort = output / "cohort"
    (cohort / "tests").mkdir(parents=True)
    (cohort / "lib").mkdir()
    for binary, name in [("owner-tickets-test", "ownerticketstest"), ("owner-tickets-parent", "testrun")]:
        shutil.copy2(root / "userland/obj/js" / binary, cohort / "tests" / name)
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
        "scope": "Private metadata only: caller supplies distinct test incarnations; no engine, runtime issuer, default-constructor or reclamation integration",
        "private_object_sha256": digest(root / "userland/obj/js/runtime/owner_tickets.o"),
        "source_files": {name: digest(root / name) for name in [
            "userland/libjs/runtime/owner_tickets.h", "userland/libjs/runtime/owner_tickets.c",
            "userland/libjs/tests/owner_ticket_cases.h", "userland/libjs/tests/owner_tickets.c",
            "userland/libjs/tests/owner_tickets_parent.c", "userland/libjs/shared.mk",
            "userland/libjs/include/os64/js.h", "userland/libjs/core.mk", "userland/GNUmakefile",
            "tools/test_js_owner_tickets_host.sh", "tools/test_js_owner_tickets_guest.py"]},
        "boot_sha256": digest(output / "boot.iso"),
        "base_image_sha256": digest(args.base_image.resolve()),
        "files": len(files), "bytes": sum(p.stat().st_size for p in files),
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    command = ["qemu-system-x86_64", "-machine", "q35", "-m", "2G", "-smp", "2",
        "-display", "none", "-nic", "none", "-no-reboot", "-boot", "d", "-cdrom",
        str(output / "boot.iso"), "-drive", "file=" + str(output / "os64.img") + ",format=raw,if=none,id=nvme0",
        "-device", "nvme,drive=nvme0,serial=OWNERTICKETS", "-serial", "stdio", "-monitor", "none"]
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
                if b"OWNER TICKETS QEMU COMPLETE PASS" in tail or b"OWNER TICKETS QEMU COMPLETE FAIL" in tail:
                    passed = b"OWNER TICKETS QEMU COMPLETE PASS" in tail
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
    print("OWNER TICKETS QEMU PASS" if passed else "OWNER TICKETS QEMU missing/failed parent verdict", flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
