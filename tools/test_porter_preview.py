#!/usr/bin/env python3
"""Exercise read-only Porter preview with disposable Os64 disks and a local valet.

Build the stock ISO and data disk first. The default ext2 run checks the model,
and --scenario fat boots the real FAT lifeboat entry with OVMF. Other scenarios
seed different read-only receipt snapshots. The scratch directory keeps logs.
"""

import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import re
import shutil
import socket
from socketserver import StreamRequestHandler, ThreadingTCPServer
import subprocess as sp
import sys
import tempfile
from threading import Barrier, Thread
import time

from porter_store import canonical, check_manifest

FAT = "2f4fd02e-68b4-4c82-98bc-72467529b3fc"
EXT2 = "1ec5f5ab-71b7-45cd-a7a4-05646e878e57"
HOME = "7a3c1d90-4e62-4f3b-9a55-0c6f2b8e41d7"
FMT = "2026-11-24T22:25:00Z"


class QMP:
    def __init__(self, path):
        self.conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.conn.settimeout(7)
        self.conn.connect(str(path))
        self.stream = self.conn.makefile("rwb")
        assert "QMP" in json.loads(self.stream.readline())
        self.send("qmp_capabilities")

    def send(self, command, arguments=None):
        self.stream.write(json.dumps({"execute": command, "arguments": arguments or {}}).encode() + b"\n")
        self.stream.flush()
        while line := self.stream.readline():
            response = json.loads(line)
            if "return" in response:
                return response["return"]
            if "error" in response:
                raise RuntimeError(response)
        raise RuntimeError(f"QMP connection closed during {command}")

    def type(self, command):
        names = {"/": "slash", " ": "spc", "-": "minus", ".": "dot"}
        for key in [*(names.get(c, c) for c in command), "ret"]:
            self.send("human-monitor-command", {"command-line": f"sendkey {key}"})
            time.sleep(0.13)

    def close(self):
        self.stream.close()
        self.conn.close()


def wait_for(process, fn, timeout=85):
    started = time.monotonic()
    while time.monotonic() - started < timeout:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited {process.returncode}")
        if fn():
            return
        time.sleep(0.5)
    raise RuntimeError("QEMU condition timed out")



def run(*args, **kw):
    return sp.run(args, check=True, text=True, capture_output=True, **kw).stdout


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def file(lot, path, data):
    return dict(kind="file", lot=lot, path=path, sha256=sha(data), size=len(data))


def directory(lot, path):
    return dict(kind="dir", lot=lot, path=path)


def save(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def publish(kind, label, lots, entries, baseline=False, kernel=None, publication=None):
    repo = BASE / "repo"
    obj = dict(version=1, kernel=None if kernel is None else dict(elf_sha256=kernel),
               scope=kind, lots=sorted(lots), entries=sorted(entries, key=lambda x: (x["lot"], x["path"])))
    check_manifest(obj)
    raw = canonical(obj)
    target = sha(raw)
    ident = publication or sha(label.encode())[:32]
    save(repo / "manifests" / (target + ".json"), raw)
    pub = dict(version=1, id=ident, name=label, baseline=baseline, published_at=FMT,
               targets={kind: target}, revision="fixture", dirty=False, branch="fixture",
               input_rules={}, toolchain={})
    save(repo / "publications" / (ident + ".json"), canonical(pub))
    return ident, target


def receipt(scope, target, entries, previous=None):
    raw = canonical(dict(version=1, scope=scope, target=target, previous=previous,
                         entries=sorted(entries, key=lambda x: (x["lot"], x["path"]))))
    ident = sha(raw)
    save(BASE / "home-files/.porter/receipts" / (ident + ".json"), raw)
    return ident


def active(scope, ident, volumes):
    stamp = (ident + "\n").encode()
    save(BASE / "home-files/.porter/heads" / scope, stamp)
    for volume in volumes:
        save(BASE / (volume + "-files") / ".porter/stamps" / scope, stamp)


def inject_ext(img, files):
    commands = []
    for p in sorted(files.rglob("*")):
        path = "/" + p.relative_to(files).as_posix()
        if p.is_dir():
            commands.append(f"mkdir {path}")
        else:
            commands.append(f"write {p} {path}")
    command = BASE / (files.name + ".cmd")
    command.write_text("\n".join(commands) + "\n")
    return run("debugfs", "-w", "-f", command, img)


def media_manifests():
    # First adoption sees matching, differing, missing and unmanaged entries.
    a = [file("sounds", name, data) for name, data in
         [("same.bin", b"same"), ("new.bin", b"new"), ("diff.bin", b"desired")]]
    tests = {
        "adopt": publish("adopt", "first adoption", ["sounds", "videos"], a, True),
        "fresh": publish("fresh", "non-baseline first install", ["sounds", "videos"], a),
        "extension": publish("extension", "uppercase suffix overlay", ["downloads"], [
            directory("downloads", "nested"), file("downloads", "nested/movie.mp4", b"123")
        ], True),
        "alias": publish("alias", "actual alias", ["actual"], [
            file("actual", "FATEXI~1.BIN", b"new")
        ], True),
        "proposed": publish("proposed", "proposed alias", ["prospective"], [
            file("prospective", "Long Filename.bin", b"long"), file("prospective", "LONGFI~1.BIN", b"short")
        ], True),
        "case": publish("case", "duplicate case", ["case"], [
            file("case", "Some.bin", b"1"), file("case", "some.bin", b"2")
        ], True),
        "protected": publish("protected", "protected EFI", ["efi"], [file("efi", "BOOTX64.EFI", b"blocked")], True),
        "logs": publish("logs", "protected transcript", ["logs"], [file("logs", "chosen.log", b"blocked")], True),
        "altlogs": publish("altlogs", "protected alternate-root transcript", ["altlogs"], [
            file("altlogs", "chosen.log", b"blocked")
        ], True),
        "var": publish("var", "adjacent to transcripts", ["var"], [file("var", "porter-demo.txt", b"nearby")], True),
        "collision": publish("collision", "cross-scope claim", ["collide"], [file("collide", "kept.bin", b"kept")], True),
        "prune": publish("prune", "scoped retirements", ["pruning"], [], False),
        "big": publish("big", "FAT size check", ["prospective"], [
            dict(file("prospective", "too-large.bin", b"placeholder"), size=4294967296)
        ], True),
        "ambiguity": publish("ambiguity", "ambiguous first ID", ["videos"], [], True, publication="abcde012" + "0" * 24),
        "ambiguity2": publish("ambiguity", "ambiguous second ID", ["videos"], [], True, publication="abcde012" + "1" * 24),
        "move": publish("prune", "changed route", ["omitted"], [file("omitted", "kept.bin", b"kept")], False),
    }
    home = BASE / "home-files"
    for path, data in [("adopt/same.bin", b"same"), ("adopt/diff.bin", b"local"),
                       ("adopt/unmanaged.bin", b"outside"), ("empty/extra.bin", b"outside"),
                       ("prune/folder/keep.bin", b"unmanaged"), ("prune/folder/retired.bin", b"retired"),
                       ("omitted/kept.bin", b"kept")]:
        save(home / "proof" / path, data)
    save(BASE / "fat-files/proof/live/Fat Existing Long Name.bin", b"existing")
    prior = [directory("pruning", "folder"), file("pruning", "folder/retired.bin", b"retired"),
             file("omitted", "kept.bin", b"kept")]
    locations = {"pruning": "/proof/prune/", "omitted": "/proof/omitted/"}
    old = [dict(e, volume=HOME, relative=locations[e["lot"]] + e["path"]) for e in prior]
    ident = receipt("prune", "1" * 64, old)
    active("prune", ident, ["home"])
    return tests


def system_manifests(tests):
    kernel_bytes = (tree / "disk/deploy/kernel/bin/os64_kernel").read_bytes()
    kernel = file("boot", "os64_kernel", kernel_bytes)
    libraries = dict(image=b"image prior", sound=b"sound prior", same=b"unchanged")
    prior = [file("libs", f"lib{k}.so", v) for k, v in libraries.items()]
    audio = [dict(e) if e["path"] != "libsound.so" else file("libs", e["path"], b"new sound") for e in prior]
    integrated = [dict(e) if e["path"] != "libimage.so" else file("libs", e["path"], b"new image") for e in prior]

    def placed(entries):
        return [dict(kernel, volume=FAT, relative="/boot/os64_kernel")] + [
            dict(e, volume=EXT2, relative="/porter-proof/syslibs/" + e["path"]) for e in entries]

    pub, target = publish("system", "audio branch", ["boot", "libs"], [kernel, *audio], True, kernel["sha256"])
    tests["system"] = pub, target
    pub, target = publish("system", "integrated userland", ["boot", "libs"], [kernel, *integrated], True, kernel["sha256"])
    tests["reverse"] = pub, target
    old = receipt("system", "2" * 64, placed(prior))
    current = audio if mode == "reverse" else integrated
    ident = receipt("system", "3" * 64, placed(current), old)
    active("system", ident, ["ext", "fat"])
    libraries["sound" if mode == "reverse" else "image"] = b"new sound" if mode == "reverse" else b"new image"
    if mode == "drift":
        libraries["image"] = b"image prior"  # Same as desired, still drift from receipt.
    for k, v in libraries.items():
        save(BASE / f"ext-files/porter-proof/syslibs/lib{k}.so", v)
    if mode == "legacy":
        for loc in ("home-files/.porter/heads", "ext-files/.porter/stamps", "fat-files/.porter/stamps"):
            (BASE / loc / "system").unlink()
    if mode == "stale":
        (BASE / "fat-files/.porter/stamps/system").unlink()
    if mode == "recovery":
        save(BASE / "home-files/.porter/operation", b"operation fixture")


def snapshot_area():
    out = BASE / "home-part.img"
    run("dd", f"if={BASE / 'home.img'}", f"of={out}", "bs=1M", "skip=1", "conv=sparse", "status=none")
    return out


def contents(p):
    return {f.relative_to(p).as_posix(): sha(f.read_bytes()) if f.is_file() else "dir" for f in p.rglob("*")}


def probe_snapshot(label):
    home = snapshot_area()
    ext = BASE / (label + "-ext.img")
    run("dd", f"if={BASE / 'system.img'}", f"of={ext}", "bs=1M", f"skip={fat_mib + 1}", "count=256",
        "conv=sparse", "status=none")
    out = BASE / label
    out.mkdir()
    for source, paths in [(home, ["/proof", "/.porter", "/porter.conf"]),
                          (ext, ["/porter-proof", "/.porter"])]:
        dest = out / ("home" if source == home else "ext")
        dest.mkdir()
        for root in paths:
            run("debugfs", "-R", f"rdump {root} {dest}", source)
    fat = out / "fat"
    fat.mkdir()
    for root in ["proof", ".porter"]:
        if (BASE / "fat-files" / root).exists():
            run("mcopy", "-s", "-i", f"{BASE}/system.img@@1048576", f"::/{root}", fat)
    run("mcopy", "-i", f"{BASE}/system.img@@1048576", "::/boot/os64_kernel", fat)
    result = contents(out)
    assert "home/proof/adopt/same.bin" in result
    assert "fat/os64_kernel" in result
    return result


def prepare():
    for dest in ["home-files", "ext-files", "fat-files"]:
        (BASE / dest).mkdir()
    tests = media_manifests()
    (BASE / "repo/objects").mkdir()
    system_manifests(tests)
    overlay = """[destinations]
sounds = /home/proof/adopt
videos = /home/proof/empty
downloads = /home/proof/default
downloads/*.MP4 = /fat/proof/video
actual = /fat/proof/live
prospective = /fat/proof/proposed
case = /fat/proof/case
efi = /fat/EFI/BOOT
collide = /home/proof/omitted
pruning = /home/proof/prune
omitted = /home/proof/omitted
logs = /var/porter
altlogs = /fat/var/porter
var = /var
libs = /porter-proof/syslibs
"""
    if mode == "move":
        overlay = overlay.replace("omitted = /home/proof/omitted", "omitted = /home/proof/new-route")
    if mode == "fat":
        overlay = overlay.replace("libs = /porter-proof/syslibs", "libs = /ext2/porter-proof/syslibs")
        overlay = overlay.replace("altlogs = /fat/var/porter", "altlogs = /ext2/var/porter")
    if mode == "logfail":
        save(BASE / "ext-files/var/porter", b"existing non-directory\n")
    save(BASE / "home-files/porter.conf", overlay.encode())
    run("cp", "--sparse=always", image_tree / "disk/os64.img", BASE / "system.img")
    run("cp", "--sparse=always", image_tree / "disk/os64_data.img", BASE / "home.img")
    home = snapshot_area()
    inject_ext(home, BASE / "home-files")
    run("dd", f"if={home}", f"of={BASE / 'home.img'}", "bs=1M", "seek=1", "conv=notrunc,sparse", "status=none")
    ext = BASE / "inject-ext.img"
    run("cp", "--sparse=always", image_tree / "disk/ext2_test.img", ext)
    if mode == "legacy":
        save(BASE / "ext-files/bin/porter", (tree / "disk/deploy/userland/bin/porter").read_bytes())
        save(BASE / "ext-files/etc/porter.conf", (tree / "etc/porter.conf").read_bytes())
    inject_ext(ext, BASE / "ext-files")
    run("dd", f"if={ext}", f"of={BASE / 'system.img'}", "bs=1M", f"seek={fat_mib + 1}",
        "conv=notrunc,sparse", "status=none")
    for item in (BASE / "fat-files").iterdir():
        run("mcopy", "-s", "-i", f"{BASE}/system.img@@1048576", item, "::/")
    return tests


@contextmanager
def partial_catalog(tests):
    if mode not in ("main", "fat"):
        yield None
        return
    pub = BASE / "repo/publications" / (tests["adopt"][0] + ".json")
    reply = b"OK\n" + pub.read_bytes() + b"\n"
    together = Barrier(2, timeout=20)

    class PartialCatalog(StreamRequestHandler):
        def handle(self):
            assert self.rfile.readline(64) == b"CATALOG\n"
            # Both Porter tasks have their transcript open before either
            # receives its first row. Closing without the terminator fails.
            together.wait()
            self.wfile.write(reply)

    with ThreadingTCPServer(("127.0.0.1", 0), PartialCatalog) as server:
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield server.server_address[1]
        finally:
            server.shutdown()
            thread.join()


def guest(tests, port, partial_port):
    # Mirror the production commands. The status and completion files belong
    # to the harness, outside the scanned/snapshotted destinations.
    s = ["cat /sys/mounts > /home/proof-mounts"]
    fat_device = f"{BASE}/system.img@@1048576"
    if partial_port:
        for name, command in (("mixed-a", "porter"), ("mixed-b", "/bin/porter")):
            script = BASE / (name + ".sh")
            script.write_text(
                f"{command} catalog 10.0.2.2 --port {partial_port}"
                f" > /home/proof-{name} 2> /home/proof-{name}-err\n"
                f"echo $? > /home/proof-{name}-status\n"
                "sync\n")
            run("mcopy", "-o", "-i", fat_device, script, f"::/{name}.sh")
        mount = "" if mode == "fat" else "/fat"
        s += [f"husk {mount}/mixed-a.sh &", f"husk {mount}/mixed-b.sh"]
    s += [
        f"porter catalog 10.0.2.2 --port {port} 2> /home/proof-catalog-err | cat > /home/proof-catalog",
        "echo catalog $? >> /home/proof-status",
        r"""porter "bad command" 'quote" slash\' "" > /home/proof-bad 2> /home/proof-bad-err""",
        "echo bad $? >> /home/proof-status",
    ]
    cases = ["adopt", "fresh", "extension", "alias", "proposed", "case", "protected",
             "logs", "altlogs", "var", "collision", "prune", "big", "ambiguity", "system"]
    if mode == "legacy":
        cases = ["adopt", "system"]
    if mode == "logfail":
        cases = ["adopt"]
    if mode == "move":
        cases = ["move"]
    if mode == "reverse":
        cases = ["reverse"]
    if mode in ("drift", "stale", "recovery"):
        cases = ["system"]
    for label in cases + (["readopt"] if mode in ("drift", "stale") else []):
        lookup = "system" if label == "readopt" else label
        ident, _ = tests[lookup]
        scope = "system" if lookup in ("system", "reverse") else "prune" if lookup == "move" else lookup
        if lookup in ("adopt", "ambiguity"):
            ident = ident[:8]
        option = " --adopt" if label == "readopt" else ""
        s.append(f"porter preview 10.0.2.2 {ident} {scope} --port {port}{option}"
                 f" > /home/proof-{label} 2> /home/proof-{label}-err")
        s.append(f"echo {label} $? >> /home/proof-status")
    s += ["sync", "echo ready > /home/proof-done", "sync"]
    script = BASE / "p.sh"
    script.write_text("\n".join(s) + "\n")
    run("mcopy", "-o", "-i", f"{BASE}/system.img@@1048576", script, "::/p.sh")


def check():
    results = {}
    for path in ["status", "catalog", "mounts", "adopt", "fresh", "extension", "alias", "proposed",
                 "case", "protected", "logs", "altlogs", "var", "collision", "prune", "big", "ambiguity", "move", "system", "reverse", "readopt"]:
        raw = run("debugfs", "-R", f"cat /proof-{path}", BASE / "home-part.img")
        (BASE / f"{path}.txt").write_text(raw)
        results[path] = raw
    print(results["mounts"], results["status"], sep="")
    for k, value in results.items():
        if value and k not in ("status", "mounts", "catalog"):
            print(k, *(l for l in value.splitlines() if l.startswith(("BLOCKED", "ADD", "ADOPT",
                  "REPLACE", "CONFLICT", "DOWNGRADE", "UNMANAGED", "RETIRE", "PRESERVE", "Kernel"))), sep="\n")
    statuses = {name: int(code) for name, code in re.findall(r"^(\w+) (\d+)$", results["status"], re.M)}
    assert statuses.get("catalog") == 0
    assert statuses.get("bad") == 2
    expected = {
        "adopt": ["ADD /home/proof/adopt/new.bin", "ADOPT /home/proof/adopt/same.bin",
                  "REPLACE /home/proof/adopt/diff.bin", "UNMANAGED /home/proof/adopt/unmanaged.bin",
                  "UNMANAGED /home/proof/empty/extra.bin"],
        "fresh": ["first adoption requires a baseline"],
        "extension": [
            "ADD " + ("" if mode == "fat" else "/fat") + "/proof/video/nested/movie.mp4",
            "ADD " + ("" if mode == "fat" else "/fat") + "/proof/video/nested  ",
            "ADD " + ("" if mode == "fat" else "/fat") + "/proof/video  ",
            "ADD /home/proof/default  ",
        ],
        "alias": ["existing FAT short-name alias"],
        "proposed": ["proposed FAT long name and numeric short alias"],
        "case": ["selected entries resolve to the same destination"],
        "protected": ["destination is reserved"],
        "logs": ["CONFLICT /var/porter/chosen.log  (destination is reserved)"],
        "altlogs": ["CONFLICT " + ("/ext2" if mode == "fat" else "/fat") +
                    "/var/porter/chosen.log  (destination is reserved)"],
        "var": ["ADD /var/porter-demo.txt", "Preview complete. Managed files and install history were left untouched."],
        "collision": ["overlaps another scope"],
        "prune": ["RETIRE /home/proof/prune/folder/retired.bin", "PRESERVE /home/proof/prune/folder",
                  "PRESERVE /home/proof/omitted/kept.bin"],
        "big": ["file exceeds the FAT size limit"],
        "ambiguity": [],
        "system": ["DOWNGRADE " + ("/ext2" if mode == "fat" else "") + "/porter-proof/syslibs/libimage.so",
                   "REPLACE " + ("/ext2" if mode == "fat" else "") + "/porter-proof/syslibs/libsound.so",
                   "target matches booted and staged kernel"],
    }
    if mode == "logfail":
        expected = {"adopt": expected["adopt"]}
    if mode == "legacy":
        expected = {"adopt": [*expected["adopt"]], "system": ["unknown booted kernel identity"]}
    if mode == "move":
        expected = {"move": ["CONFLICT /home/proof/new-route/kept.bin", "PRESERVE /home/proof/omitted/kept.bin"]}
    if mode == "reverse":
        expected = {"reverse": ["DOWNGRADE /porter-proof/syslibs/libsound.so", "REPLACE /porter-proof/syslibs/libimage.so"]}
    if mode == "drift":
        expected = {"system": ["CONFLICT /porter-proof/syslibs/libimage.so", "local drift"],
                    "readopt": ["ADOPT /porter-proof/syslibs/libimage.so", "REPLACE /porter-proof/syslibs/libsound.so"]}
    if mode == "stale":
        expected = {"system": ["receipt or volume stamp is missing"],
                    "readopt": ["baseline adoption", "REPLACE /porter-proof/syslibs/libimage.so"]}
    if mode == "recovery":
        expected = {"system": ["operation needs recovery before preview"]}
    for name, strings in expected.items():
        for value in strings:
            assert value in results[name], (name, value, results[name])
        fail = name in ("fresh", "alias", "proposed", "case", "protected", "logs", "altlogs", "collision", "big", "ambiguity", "move") or (
            name == "system" and mode in ("drift", "stale", "recovery", "legacy"))
        assert statuses.get(name) == int(fail), (name, statuses)


    assert "UNMANAGED /var/porter" not in results["var"], results["var"]
    for label in ("logs", "altlogs"):
        if results[label]:
            rows = re.findall(r"^(?:ADD|ADOPT|UNMANAGED|CONFLICT) /.*$", results[label], re.M)
            assert len(rows) == 1 and rows[0].startswith("CONFLICT "), (label, rows)
    return statuses


def check_transcripts(statuses, port, partial_port):
    def guest_file(label):
        return run("debugfs", "-R", f"cat /proof-{label}", BASE / "home-part.img")

    if mode == "logfail":
        assert guest_file("catalog") and "first adoption" in guest_file("catalog")
        for label in ("catalog", "adopt", "bad"):
            assert "could not start a transcript in /var/porter" in guest_file(label + "-err")
        assert "Usage: porter" in guest_file("bad-err")
        sentinel = run("debugfs", "-R", "cat /var/porter", BASE / "after-ext.img")
        assert sentinel == "existing non-directory\n", sentinel
        print("Unavailable transcript directory preserved stdout, diagnostics and exit codes")
        return

    transcripts = BASE / "transcripts"
    transcripts.mkdir()
    if mode == "fat":
        run("mcopy", "-s", "-i", f"{BASE}/system.img@@1048576", "::/var/porter", transcripts)
    else:
        run("debugfs", "-R", f"rdump /var/porter {transcripts}", BASE / "after-ext.img")
    logs = {p.name: p.read_text() for p in (transcripts / "porter").iterdir()}
    assert len(logs) == len(statuses) + (2 if partial_port else 0), list(logs)
    assert all(re.fullmatch(r"run-\d{8}-\d{6}Z-\d+-\d+\.log", name) for name in logs)
    used = set()

    def checked(label, code, command=None, tail_error=False):
        err = guest_file(label + "-err")
        notice, _, errors = err.partition("\n")
        assert notice.startswith("porter: transcript /var/porter/"), (label, err)
        name = notice.rsplit("/", 1)[1]
        assert name not in used
        used.add(name)
        invocation, _, body = logs[name].partition("\n")
        if command:
            assert invocation == "Command:" + "".join(" " + json.dumps(arg) for arg in command), invocation
        assert body == (notice + "\n" + guest_file(label) + errors + f"\nExit: {code}\n"), (label, body, err)
        if tail_error:
            assert guest_file(label) and errors == "porter: cannot read or verify catalog\n", (label, err)

    checked("catalog", 0, ["porter", "catalog", "10.0.2.2", "--port", str(port)])
    checked("bad", 2, ["porter", "bad command", 'quote" slash\\', ""])
    for label, status in statuses.items():
        if label not in ("catalog", "bad"):
            checked(label, status)
    if partial_port:
        for label, executable in (("mixed-a", "porter"), ("mixed-b", "/bin/porter")):
            assert guest_file(label + "-status") == "1\n"
            checked(label, 1, [executable, "catalog", "10.0.2.2", "--port", str(partial_port)], tail_error=True)
    print(f"Verified {len(logs)} distinct transcripts against live stdout, stderr and exit codes")



def main(tests, partial_port):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    guest(tests, port, partial_port)
    before = probe_snapshot("before")
    arch = "qemu64,+rdrand,+rdseed,+ssse3,+sse4.1,+sse4.2,+popcnt,+xsave,+xsaveopt,+avx,+avx2,+fma,+bmi1,+bmi2"
    cmd = ["qemu-system-x86_64", "-machine", "q35", "-cpu", arch, "-m", "8g", "-smp", "4",
           "-no-reboot", "-display", "none", "-monitor", "none", "-qmp", f"unix:{BASE}/qmp,server=on,wait=off",
           "-serial", f"file:{BASE}/serial.log", "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0",
           "-drive", f"file={BASE}/system.img,format=raw,if=none,id=os64", "-device", "nvme,drive=os64,serial=nvme-sys",
           "-drive", f"file={BASE}/home.img,format=raw,if=none,id=home", "-device", "nvme,drive=home,serial=nvme-home"]
    # Boot the real FAT-root Limine entry by selecting the existing menu entry.
    if mode == "fat":
        conf = (tree / "limine-hd.conf").read_text()
        start = conf.index("/os64 (FAT")
        end = conf.find("\n/", start + 1)
        entry = conf[start:] if end < 0 else conf[start:end]
        (BASE / "fat.conf").write_text("timeout: 0\n" + entry + "\n")
        run("mcopy", "-o", "-i", f"{BASE}/system.img@@1048576", BASE / "fat.conf", "::/boot/limine/limine.conf")
        vars_ = BASE / "vars.fd"
        shutil.copyfile(args.ovmf_vars, vars_)
        cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={args.ovmf_code}",
                "-drive", f"if=pflash,format=raw,file={vars_}"]
    else:
        cmd += ["-cdrom", str(image_tree / "os64_kernel.iso"), "-boot", "d"]
    with (BASE / "server.log").open("w") as slog, (BASE / "qemu.log").open("w") as qlog:
        source = BASE / "fat-files"
        server = sp.Popen([sys.executable, "-u", str(tree / "tools/os64serve.py"), str(source),
                          "--bind", "127.0.0.1", "--port", str(port),
                          "--porter-repo", str(BASE / "repo")], stdout=slog, stderr=slog)
        machine = None
        try:
            wait_for(server, lambda: "serving on" in (BASE / "server.log").read_text().lower())
            machine = sp.Popen(cmd, stdout=qlog, stderr=qlog)
            serial = BASE / "serial.log"
            # Two boot consoles can interleave with the pool's serial message.
            wait_for(machine, lambda: serial.is_file() and re.search(
                r"\d+/\d+ paging pages used", serial.read_text(errors="replace")))
            time.sleep(2)
            q = QMP(BASE / "qmp")
            try:
                q.type("husk /p.sh" if mode == "fat" else "husk /fat/p.sh")
                def done():
                    image = str(BASE / "home.img") + "?offset=1048576"
                    if run("debugfs", "-R", "cat /proof-done", image).strip() != "ready":
                        return False
                    return not partial_port or all(
                        run("debugfs", "-R", f"cat /proof-{label}-status", image).strip()
                        for label in ("mixed-a", "mixed-b"))
                wait_for(machine, done, timeout=180)
                q.send("quit")
                assert machine.wait(timeout=12) == 0
            finally:
                q.close()
        finally:
            if machine and machine.poll() is None:
                machine.terminate()
                machine.wait(timeout=10)
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=10)
    after = probe_snapshot("after")
    assert after == before, (before.keys() ^ after.keys(),
                            [k for k in before.keys() & after.keys() if before[k] != after[k]])
    print(f"{len(after)} managed and neighboring files/directories and state entries unchanged")
    check_transcripts(check(), port, partial_port)


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, default=Path(__file__).resolve().parents[1],
                        help="Os64 checkout with the default disk targets already built")
    parser.add_argument("--scenario", default="main",
                        choices=("main", "fat", "reverse", "drift", "stale", "recovery", "move", "legacy", "logfail"))
    parser.add_argument("--scratch", type=Path,
                        help="parent directory for disposable disk images and logs")
    parser.add_argument("--legacy-tree", type=Path,
                        help="built checkout predating /sys/boot-kernel, for the legacy scenario")
    parser.add_argument("--ovmf-code", type=Path, default=Path("/usr/share/OVMF/OVMF_CODE_4M.fd"))
    parser.add_argument("--ovmf-vars", type=Path, default=Path("/usr/share/OVMF/OVMF_VARS_4M.fd"))
    parsed = parser.parse_args()
    if parsed.scenario == "legacy" and not parsed.legacy_tree:
        parser.error("--scenario legacy requires --legacy-tree")
    return parsed


if __name__ == "__main__":
    args = arguments()
    tree = args.tree.resolve()
    legacy_tree = args.legacy_tree.resolve() if args.legacy_tree else None
    mode = args.scenario
    image_tree = legacy_tree if mode == "legacy" else tree
    info = run("sgdisk", "-i", "2", image_tree / "disk/os64.img")
    start = int(re.search(r"First sector: (\d+)", info)[1])
    if start % 2048:
        raise SystemExit("expected the ext2 partition to start on a MiB boundary")
    fat_mib = start // 2048 - 1
    BASE = Path(tempfile.mkdtemp(prefix=f"porter-{mode}-", dir=args.scratch))
    print(f"Porter {mode} scratch directory {BASE}", flush=True)
    tests = prepare()
    with partial_catalog(tests) as partial_port:
        main(tests, partial_port)
