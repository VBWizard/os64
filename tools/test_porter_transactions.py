#!/usr/bin/env python3
"""Exercise Porter transactions on disposable ext2 QEMU disks.

Run after building the ISO and data disk. Interruption cases accept a Porter
binary built with -DPORTER_TEST_INTERRUPTS. The normal system image can keep
its production Porter binary. Scratch images are removed when the scenario exits.
"""

import argparse
from contextlib import contextmanager
from pathlib import Path
import re
import socket
import subprocess as sp
import sys
from tempfile import TemporaryDirectory
import time

from porter_store import canonical, check_manifest, gzip_member, object_path, padding
from test_porter_preview import QMP, run, sha, wait_for

CPU = "qemu64,+rdrand,+rdseed,+ssse3,+sse4.1,+sse4.2,+popcnt,+xsave,+xsaveopt,+avx,+avx2,+fma,+bmi1,+bmi2"
OLD_SOUND = b"local original sound\n"
OLD_IMAGE = b"base image\n"
BASE_SOUND = b"base sound\n"
AUDIO_SOUND = b"audio preview\n"
YONDER_IMAGE = b"yonder preview\n"
RETIRING = b"retire from later scope\n"


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def file(lot, name, data):
    return dict(kind="file", lot=lot, path=name, sha256=sha(data), size=len(data))


def dir_entry(lot, name):
    return dict(kind="dir", lot=lot, path=name)


class Fixture:
    def __init__(self, tree, root, instrumented):
        self.tree, self.root = tree, root
        self.repo, self.system, self.home = root / "repo", root / "system.img", root / "home.img"
        self.system_fs, self.home_fs = root / "root-ext2.img", root / "home-ext2.img"
        self.serial = root / "serial.txt"
        self.pubs = {}
        self.boots = 0
        self.port = None
        (self.repo / "objects").mkdir(parents=True)

        kernel = (tree / "disk/deploy/kernel/bin/os64_kernel").read_bytes()
        k = file("boot", "os64_kernel", kernel)
        for name, sound, image, retire in (
            ("base", BASE_SOUND, OLD_IMAGE, True),
            ("audio", AUDIO_SOUND, OLD_IMAGE, True),
            ("yonder", BASE_SOUND, YONDER_IMAGE, False),
        ):
            files = [(k, kernel), (file("libs", "libsound.so", sound), sound),
                     (file("libs", "libimage.so", image), image)]
            if retire:
                files.append((file("libs", "retiring.so", RETIRING), RETIRING))
            self.publish(name, "system", files, name == "base", k["sha256"])
        self.publish("owner-a", "owner-a", [(file("sharedfiles", "a", b"owned by A\n"), b"owned by A\n")], True)
        self.publish("owner-b", "owner-b", [(dir_entry("shareroot", "shared"), None)], True)

        run("cp", "--sparse=always", tree / "disk/os64.img", self.system)
        run("cp", "--sparse=always", tree / "disk/os64_data.img", self.home)
        run("cp", "--sparse=always", tree / "disk/ext2_test.img", self.system_fs)
        run("dd", f"if={self.home}", f"of={self.home_fs}", "bs=1M", "skip=1", "conv=sparse", "status=none")
        self.fs_write(self.system_fs, "/porter-proof", None)
        self.fs_write(self.system_fs, "/porter-proof/lib", None)
        self.fs_write(self.system_fs, "/porter-proof/lib/libsound.so", OLD_SOUND)
        self.fs_write(self.system_fs, "/porter-proof/lib/libimage.so", OLD_IMAGE)
        self.fs_write(self.system_fs, "/porter-proof/lib/unmanaged", b"keep me\n")
        self.fs_write(self.home_fs, "/porter.conf", b"[destinations]\nlibs = /porter-proof/lib\n"
                      b"sharedfiles = /home/shared\nshareroot = /home\n")
        if instrumented:
            run("debugfs", "-w", "-R", "rm /bin/porter", self.system_fs)
            self.fs_write(self.system_fs, "/bin/porter", instrumented.read_bytes())
        start = int(re.search(r"First sector: (\d+)", run("sgdisk", "-i", "2", self.system))[1])
        assert start % 2048 == 0
        self.ext_offset = start * 512
        for fs, disk, start_at in ((self.system_fs, self.system, self.ext_offset),
                                   (self.home_fs, self.home, 1048576)):
            run("dd", f"if={fs}", f"of={disk}", "bs=1M", f"seek={start_at // 1048576}",
                "conv=notrunc,sparse", "status=none")
            fs.unlink()

    def publish(self, name, scope, files, baseline, kernel=None):
        entries = sorted((entry for entry, _ in files), key=lambda e: (e["lot"], e["path"]))
        target = dict(version=1, scope=scope, lots=sorted({e["lot"] for e in entries}), entries=entries,
                      kernel=dict(elf_sha256=kernel) if kernel else None)
        check_manifest(target)
        raw = canonical(target)
        digest = sha(raw)
        for entry, data in files:
            if data is not None:
                save(object_path(self.repo, entry["sha256"]), gzip_member(data + bytes(padding(len(data)))))
        save(self.repo / "manifests" / (digest + ".json"), raw)
        ident = sha(name.encode())[:32]
        pub = dict(version=1, id=ident, name=name, baseline=baseline, published_at="2026-11-25T12:00:00Z",
                   branch="fixture", revision="fixture", dirty=False, input_rules={}, toolchain={},
                   targets={scope: digest})
        save(self.repo / "publications" / (ident + ".json"), canonical(pub))
        self.pubs[name] = ident, scope, digest

    def fs_write(self, image, name, data):
        src = self.root / "filesystem-input"
        if data is None:
            command = "mkdir " + name
        else:
            src.write_bytes(data)
            command = f"write {src} {name}"
        run("debugfs", "-w", "-R", command, image)
        metadata = run("debugfs", "-R", f"stat {name}", image)
        assert "Inode:" in metadata, (image, name, metadata)
        if data is not None:
            src.unlink()

    def read(self, path, raw=False, root=False):
        image, offset = (self.system, self.ext_offset) if root else (self.home, 1048576)
        command = f"{'stat' if raw else 'cat'} {path}"
        return run("debugfs", "-R", command, f"{image}?offset={offset}")

    def read_lib(self, name):
        return self.read("/porter-proof/lib/" + name, root=True).encode()

    def script(self, name, lines):
        src = self.root / (name + ".sh")
        src.write_text("\n".join(lines) + "\n")
        run("mcopy", "-o", "-i", f"{self.system}@@1048576", src, "::/" + src.name)

    def install(self, target, flags=""):
        pub, scope, _ = self.pubs[target]
        return f"porter install 10.0.2.2 {pub[:8]} {scope} --yes --port {self.port} {flags}".rstrip()

    def log(self, name, command):
        return [f"{command} > /home/{name}.out 2> /home/{name}.err",
                f"echo {name} $? >> /home/status",
                "sync"]

    def outcome(self, name, code, witness):
        output = self.read(f"/{name}.out") + self.read(f"/{name}.err")
        assert f"{name} {code}" in self.read("/status").splitlines(), (name, output, self.read("/status"))
        assert witness in output, (name, output)
        return output

    @contextmanager
    def serve(self):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        with (self.root / "valet.txt").open("w") as log:
            server = sp.Popen([sys.executable, "-B", "-u", self.tree / "tools/os64serve.py",
                               "--bind", "127.0.0.1", "--port", str(self.port),
                               "--porter-repo", self.repo], stdout=log, stderr=log)
            try:
                wait_for(server, lambda: "serving on" in (self.root / "valet.txt").read_text())
                yield
            finally:
                server.terminate()
                server.wait(timeout=15)

    @contextmanager
    def boot(self, *, online=True, fault=None):
        self.boots += 1
        sock = self.root / f"qmp{self.boots}"
        self.serial = self.root / f"serial-{self.boots}.txt"
        command = ["qemu-system-x86_64", "-machine", "q35", "-cpu", CPU, "-m", "8g", "-smp", "4",
                   "-cdrom", str(self.tree / "os64_kernel.iso"), "-boot", "d", "-no-reboot",
                   "-display", "none", "-monitor", "none", "-qmp", f"unix:{sock},server=on,wait=off",
                   "-serial", f"file:{self.serial}"]
        for label, path in (("os64", self.system), ("home", self.home)):
            source = f"blkdebug:{fault[1]}:{path}" if fault and label == fault[0] else str(path)
            command += ["-drive", f"file={source},format=raw,cache=writeback,werror=report,if=none,id={label}",
                        "-device", f"nvme,drive={label},serial=nvme-{label}"]
        if online:
            command += ["-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0"]
        with (self.root / "qemu.txt").open("a") as qlog:
            process = sp.Popen(command, stdout=qlog, stderr=qlog)
            try:
                wait_for(process, lambda: self.serial.exists() and re.search(
                    r"\d+/\d+ paging pages used", self.serial.read_text(errors="replace")))
                time.sleep(2)
                qmp = QMP(sock)
                try:
                    yield qmp, process
                    if process.poll() is None:
                        qmp.send("quit")
                        assert process.wait(timeout=12) == 0
                finally:
                    qmp.close()
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)

    def execute(self, qmp, machine, script, filename, text):
        qmp.type("husk /fat/" + script + ".sh")
        wait_for(machine, lambda: text in self.read(filename), timeout=180)


def switches(f):
    with f.serve():
        f.script("p", [
            *f.log("baseline", f.install("base")),
            "cat /porter-proof/lib/libsound.so > /home/base-sound",
            *f.log("audio", f.install("audio")),
            "cat /porter-proof/lib/libsound.so > /home/audio-sound",
            *f.log("preview-yonder", f.install("yonder").replace("install", "preview", 1).replace(" --yes", "")),
            *f.log("yonder", f.install("yonder")),
            "cat /porter-proof/lib/libimage.so > /home/yonder-image",
            "cat /porter-proof/lib/libsound.so > /home/yonder-sound",
            *f.log("yonder-retired", "cat /porter-proof/lib/retiring.so"),
            *f.log("same", f.install("yonder")),
            *f.log("history", "porter history"),
            *f.log("undo-yonder", "porter undo --yes"),
            "cat /porter-proof/lib/libsound.so > /home/restored-audio",
            "cat /porter-proof/lib/libimage.so > /home/restored-image",
            "cat /porter-proof/lib/retiring.so > /home/restored-retired",
            *f.log("undo-audio", "porter undo --yes"),
            *f.log("undo-base", "porter undo --yes"),
            *f.log("reinstall", f.install("base")),
            *f.log("readopt", f.install("base", "--adopt")),
            *f.log("undo-adopt", "porter undo --yes"),
            *f.log("disowned", "porter history"),
            *f.log("owner-a", f.install("owner-a")),
            *f.log("owner-b", f.install("owner-b")),
            *f.log("owner-undo", "porter undo owner-a --yes"),
            *f.log("owner-history", "porter history owner-b"),
            "echo complete > /home/done", "sync",
        ])
        with f.boot() as (q, machine):
            f.execute(q, machine, "p", "/done", "complete")
    for name in ("baseline", "audio", "yonder", "reinstall", "readopt", "owner-a", "owner-b"):
        f.outcome(name, 0, "install committed")
    for name in ("undo-yonder", "undo-audio", "undo-base", "undo-adopt", "owner-undo"):
        f.outcome(name, 0, "undo committed")
    f.outcome("same", 0, "already installed")
    assert "BLOCKED" not in f.outcome("preview-yonder", 0, "Preview complete")
    f.outcome("yonder-retired", 1, "retiring.so")
    assert f.outcome("history", 0, "ACTIVE").count("UNDOABLE") == 2
    old = f.outcome("disowned", 0, "no active receipt")
    assert "INACTIVE" in old and "UNDOABLE" not in old
    f.outcome("owner-history", 0, "ACTIVE")
    for name, expected in (("base-sound", BASE_SOUND), ("audio-sound", AUDIO_SOUND),
                           ("yonder-image", YONDER_IMAGE), ("yonder-sound", BASE_SOUND),
                           ("restored-audio", AUDIO_SOUND), ("restored-image", OLD_IMAGE),
                           ("restored-retired", RETIRING)):
        assert f.read("/" + name).encode() == expected, name
    assert "Type: directory" in f.read("/shared", raw=True)
    assert not f.read("/shared/a")
    assert f.read_lib("unmanaged") == b"keep me\n"
    assert "Inode:" not in f.read("/.porter/operation", raw=True)
    print("Direct baseline/audio/yonder switches and single undos passed, including disowned chains and a foreign directory")


def interrupt(f, phase, undo=False, torn=False):
    with f.serve():
        first = f.log("initial", f.install("base")) if undo else []
        action = "porter undo --yes" if undo else f.install("base")
        f.script("cut", [*first, f"export PORTER_TEST_CUT={phase}", *f.log("cut", action)])
        f.script("recover", [*f.log("history", "porter history"), *f.log("recover", "porter recover"),
                             *f.log("again", "porter recover"), "echo complete > /home/done", "sync"])
        with f.boot() as (q, machine):
            f.execute(q, machine, "cut", "/cut.out", f"Porter test cut {phase}")
            machine.kill()
            machine.wait(timeout=12)
    if phase == "armed" and not undo:
        operation, _ = f.read("/.porter/operation").split()
        f.script("recover", [f"mkdir /.porter/stage/{operation}/b0",
                             *f.log("recover", "porter recover"), *f.log("again", "porter recover"),
                             "echo complete > /home/done", "sync"])
    if torn:
        operation, _ = f.read("/.porter/operation").split()
        f.script("recover", [f"echo incomplete > /home/.porter/operations/{operation}/complete",
                             *f.log("recover", "porter recover"), *f.log("again", "porter recover"),
                             "echo complete > /home/done", "sync"])
    with f.boot(online=False) as (q, machine):
        f.execute(q, machine, "recover", "/done", "complete")
    completed = not torn and phase in ("complete", "head", "before-pointer-unlink", "after-pointer-unlink", "retired")
    expected = OLD_SOUND if undo or not completed else BASE_SOUND
    assert f.read_lib("libsound.so") == expected
    f.outcome("recover", 0, "no Porter operation is pending" if phase in ("before-pointer", "retired")
              else "operation" if phase == "after-pointer-unlink" else "Pending")
    f.outcome("again", 0, "no Porter operation is pending")
    if torn:
        f.outcome("recover", 0, "roll back the install")
    if phase == "armed" and not undo:
        assert "Type: directory" in f.read(f"/.porter/stage/{operation}/b0", raw=True, root=True)
    assert "Inode:" not in f.read("/.porter/operation", raw=True)
    print(f"Offline recovery passed after {'undo' if undo else 'install'} cut at {phase}" +
          (" with a torn completion record" if torn else ""))


def drift(f):
    with f.serve():
        f.script("p", ["export PORTER_TEST_PAUSE=staged", *f.log("install", f.install("base")),
                       *f.log("recover", "porter recover"), "echo complete > /home/done", "sync"])
        f.script("launch", ["husk /fat/p.sh &"])
        f.script("edit", ["echo locally edited > /porter-proof/lib/libimage.so",
                          "echo resume > /home/porter-test-release", "sync"])
        with f.boot() as (q, machine):
            q.type("husk /fat/launch.sh")
            wait_for(machine, lambda: "Porter test pause staged" in f.read("/install.out"))
            f.execute(q, machine, "edit", "/done", "complete")
    f.outcome("install", 1, "the approved before-state changed")
    f.outcome("recover", 0, "discard prepared scratch")
    assert f.read_lib("libimage.so") == b"locally edited\n"
    assert f.read_lib("libsound.so") == OLD_SOUND
    assert "Inode:" not in f.read("/.porter/operation", raw=True)
    print("An edit to an otherwise matching adopted dependency blocked arming and survived prepared cleanup")


def concurrent_recovery(f):
    with f.serve():
        f.script("cut", ["export PORTER_TEST_CUT=destinations", *f.log("cut", f.install("base"))])
        f.script("tamper", ["mkdir /home/.porter/heads", "echo unexpected > /home/.porter/heads/system",
                            *f.log("wrong-head", "porter recover"), "cat /home/.porter/heads/system > /home/untouched",
                            "rm /home/.porter/heads/system",
                            "mkdir /.porter/stamps", "echo unexpected > /.porter/stamps/system",
                            *f.log("wrong-stamp", "porter recover"), "cat /.porter/stamps/system > /home/stamp-untouched",
                            "rm /.porter/stamps/system", "echo done > /home/tampered", "sync"])
        f.script("first", ["export PORTER_TEST_PAUSE=before-pointer-unlink", *f.log("first", "porter recover"),
                           "echo complete > /home/done", "sync"])
        f.script("launch", ["husk /fat/first.sh &"])
        f.script("second", [*f.log("second", "porter recover"), "echo resume > /home/porter-test-release", "sync"])
        with f.boot() as (q, machine):
            f.execute(q, machine, "cut", "/cut.out", "Porter test cut destinations")
            machine.kill()
            machine.wait(timeout=12)
    with f.boot(online=False) as (q, machine):
        f.execute(q, machine, "tamper", "/tampered", "done")
        f.outcome("wrong-head", 1, "unexpected history head")
        assert f.read("/untouched") == "unexpected\n"
        f.outcome("wrong-stamp", 1, "unexpected history head or volume stamp")
        assert f.read("/stamp-untouched") == "unexpected\n"
        q.type("husk /fat/launch.sh")
        wait_for(machine, lambda: "Porter test pause before-pointer-unlink" in f.read("/first.out"))
        f.execute(q, machine, "second", "/done", "complete")
    f.outcome("first", 0, "recovery complete")
    f.outcome("second", 1, "another Porter writer is active")
    assert f.read_lib("libsound.so") == OLD_SOUND
    print("Unexpected metadata was preserved, and exactly one competing recovery held the writer lease")


def corrupt_transport(f):
    object_path(f.repo, sha(BASE_SOUND)).write_bytes(b"damaged object")
    with f.serve():
        f.script("p", [*f.log("install", f.install("base")), *f.log("recover", "porter recover"),
                       "echo complete > /home/done", "sync"])
        with f.boot() as (q, machine):
            f.execute(q, machine, "p", "/done", "complete")
    f.outcome("install", 1, "selected archive failed verification")
    f.outcome("recover", 0, "discard prepared scratch")
    assert f.read_lib("libsound.so") == OLD_SOUND
    assert "Inode:" not in f.read("/.porter/operation", raw=True)
    print("Corrupt archive caused prepared-only cleanup without changing the destination")


def mixed_state(f, undo):
    with f.serve():
        prepare = [*f.log("initial", f.install("base")), *f.log("audio", f.install("audio"))]
        if undo:
            prepare += f.log("yonder", f.install("yonder"))
        action = "porter undo --yes" if undo else f.install("yonder")
        f.script("cut", [*prepare, "export PORTER_TEST_CUT=first-file", *f.log("cut", action)])
        f.script("recover", [*f.log("recover", "porter recover"), "echo complete > /home/done", "sync"])
        with f.boot() as (q, machine):
            f.execute(q, machine, "cut", "/cut.out", "Porter test cut first-file")
            q.send("stop")
            image = OLD_IMAGE if undo else YONDER_IMAGE
            sound = BASE_SOUND if undo else AUDIO_SOUND
            assert f.read_lib("libimage.so") == image
            assert f.read_lib("libsound.so") == sound
            machine.kill()
            machine.wait(timeout=12)
    with f.boot(online=False) as (q, machine):
        f.execute(q, machine, "recover", "/done", "complete")
    f.outcome("recover", 0, "finish the undo" if undo else "roll back the install")
    assert f.read_lib("libimage.so") == OLD_IMAGE
    assert f.read_lib("libsound.so") == AUDIO_SOUND
    assert f.read_lib("retiring.so") == RETIRING
    print(f"Verified two different live states before the {'undo' if undo else 'install'} cut, then restored audio offline")


def disk_fault(f, mode):
    cfg = f.root / "blkdebug.conf"
    if mode == "write":
        block = int(re.search(r"\(0\):(\d+)", f.read("/porter-proof/lib", raw=True, root=True))[1])
        sector = f.ext_offset // 512 + 2 * block
        cfg.write_text(f'[inject-error]\nevent = "write_aio"\nsector = "{sector}"\nerrno = "5"\nonce = "on"\n')
        fault_drive = "os64"
    else:
        cfg.write_text("".join(f'[set-state]\nevent = "flush_to_disk"\nstate = "{i}"\nnew_state = "{i+1}"\n\n'
                               for i in range(1, 3)) +
                       '[inject-error]\nevent = "flush_to_disk"\nstate = "3"\nerrno = "5"\nonce = "on"\n')
        fault_drive = "home"
    with f.serve():
        f.script("p", [*f.log("failed", f.install("base")), "echo complete > /home/failed-done", "sync"])
        f.script("recover", [*f.log("recover", "porter recover"), "echo complete > /home/done", "sync"])
        with f.boot(fault=(fault_drive, cfg)) as (q, machine):
            f.execute(q, machine, "p", "/failed-done", "complete")
        f.outcome("failed", 1, "destination operation failed" if mode == "write" else "could not make")
        assert "Inode:" in f.read("/.porter/operation", raw=True)
    with f.boot(online=False) as (q, machine):
        f.execute(q, machine, "recover", "/done", "complete")
    f.outcome("recover", 0, "roll back the install")
    assert f.read_lib("libsound.so") == OLD_SOUND
    print(f"Real NVMe {mode} completion failure reached Porter and offline recovery restored the original bytes")


def closed_pipe(f):
    with f.serve():
        f.script("p", [*f.log("pipe", f.install("base") + " | head -n 1"), *f.log("history", "porter history"),
                       "echo complete > /home/done", "sync"])
        with f.boot() as (q, machine):
            f.execute(q, machine, "p", "/done", "complete")
    f.outcome("history", 0, "ACTIVE")
    assert f.read_lib("libsound.so") == BASE_SOUND
    assert "Inode:" not in f.read("/.porter/operation", raw=True)
    print("Install committed even though the live progress reader closed early")


def legacy(f):
    with f.serve():
        preview = f.install("base").replace("install", "preview", 1).replace(" --yes", "")
        f.script("p", [*f.log("preview", preview), *f.log("install", f.install("base")),
                       *f.log("history", "porter history"), "echo complete > /home/done", "sync"])
        with f.boot() as (q, machine):
            f.execute(q, machine, "p", "/done", "complete")
    f.outcome("preview", 0, "Preview complete")
    f.outcome("install", 1, "kernel does not provide the Porter writer lease")
    f.outcome("history", 0, "no active receipt")
    assert f.read_lib("libsound.so") == OLD_SOUND
    assert "Inode:" not in f.read("/.porter/operation", raw=True)
    print("The P.4 CLI starts on the preceding kernel and preserves read-only preview and history")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--scratch", type=Path)
    parser.add_argument("--instrumented", type=Path,
                        help="override guest Porter binary for test hooks or a prior guest image")
    parser.add_argument("--scenario", default="switch",
                        choices=("switch", "drift", "before-pointer", "prepared", "armed", "destinations",
                                 "complete", "before-pointer-unlink", "after-pointer-unlink", "undo",
                                 "write", "flush", "concurrent", "corrupt", "mixed-install", "mixed-undo",
                                 "torn", "legacy", "pipe"))
    args = parser.parse_args()
    if args.scenario not in ("switch", "write", "flush", "corrupt", "pipe") and not args.instrumented:
        parser.error("this scenario needs --instrumented built with -DPORTER_TEST_INTERRUPTS")
    with TemporaryDirectory(prefix="porter-p4-", dir=args.scratch) as tmp:
        root = Path(tmp)
        print(root, flush=True)
        f = Fixture(args.tree.resolve(), root, args.instrumented)
        try:
            if args.scenario == "switch":
                switches(f)
            elif args.scenario == "legacy":
                legacy(f)
            elif args.scenario == "pipe":
                closed_pipe(f)
            elif args.scenario == "torn":
                interrupt(f, "complete", torn=True)
            elif args.scenario == "drift":
                drift(f)
            elif args.scenario == "concurrent":
                concurrent_recovery(f)
            elif args.scenario == "corrupt":
                corrupt_transport(f)
            elif args.scenario in ("mixed-install", "mixed-undo"):
                mixed_state(f, args.scenario == "mixed-undo")
            elif args.scenario in ("write", "flush"):
                disk_fault(f, args.scenario)
            else:
                interrupt(f, "destinations" if args.scenario == "undo" else args.scenario,
                          args.scenario == "undo")
        except Exception:
            for path in sorted(root.glob("*.txt")):
                print(f"--- {path.name} ---", file=sys.stderr)
                print("\n".join(path.read_text(errors="replace").splitlines()[-40:]), file=sys.stderr)
            raise


if __name__ == "__main__":
    main()
