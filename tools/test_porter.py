#!/usr/bin/env python3
"""Porter publication, wire compatibility and archive regression tests."""

from contextlib import contextmanager
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

import porter_store as store

if os.name != "nt":
    import porter_publish as publisher


TOOLS = Path(__file__).resolve().parent
CHECKOUT = TOOLS.parent


def repository(root):
    for name in ("objects", "manifests", "publications"):
        (root / name).mkdir(parents=True)
    return root


def file_entry(repo, lot, path, data):
    digest = hashlib.sha256(data).hexdigest()
    store.object_path(repo, digest).write_bytes(store.gzip_member(data + bytes(store.padding(len(data)))))
    return {"lot": lot, "path": path, "kind": "file", "sha256": digest, "size": len(data)}


def publish_fixture(repo, entries):
    manifest = {"version": 1, "scope": "media", "kernel": None, "lots": ["media"], "entries": entries}
    store.check_manifest(manifest)
    raw = store.canonical(manifest)
    target = hashlib.sha256(raw).hexdigest()
    (repo / "manifests" / f"{target}.json").write_bytes(raw)
    identifier = "1" * 32
    record = {"id": identifier, "targets": {"media": target}, "name": "Tape & clip"}
    (repo / "publications" / f"{identifier}.json").write_bytes(store.canonical(record))
    (repo / "publications" / ".incoming-catalog-test").write_bytes(b"{")
    return target, raw, record


@contextmanager
def server(repo, *legacy_dirs):
    args = [sys.executable, "-u", str(TOOLS / "os64serve.py"), "--porter-repo", str(repo),
            "--bind", "127.0.0.1", "--port", "0", *map(str, legacy_dirs)]
    with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) as proc:
        failed = False
        try:
            line = proc.stdout.readline()
            if not line.startswith("os64serve: serving on 127.0.0.1:"):
                raise AssertionError(f"server startup: {line}")
            yield int(line.rsplit(":", 1)[-1])
        except BaseException:
            failed = True
            raise
        finally:
            if proc.poll() is None:
                proc.terminate()
            output, _ = proc.communicate(timeout=10)
            if failed:
                print(output, file=sys.stderr, end="")


def request(port, line, body=b""):
    with socket.create_connection(("127.0.0.1", port), timeout=15) as conn:
        conn.sendall(line.encode("ascii") + b"\n" + body)
        conn.shutdown(socket.SHUT_WR)
        with conn.makefile("rb") as reply:
            return reply.readline(), reply.read()


def archive_payload(header, body):
    status, size = header.split()
    if status != b"OK" or int(size) != len(body):
        raise AssertionError((header, len(body)))
    return body


class LoopbackTests(unittest.TestCase):
    """Run this class on native Windows as well as Linux."""

    def test_catalog_manifest_full_selective_and_legacy(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repo = repository(root / "Porter repo")
            legacy = root / "Legacy bin"
            legacy.mkdir()
            (legacy / "hello").write_bytes(b"hello")
            samples = {"0 bytes": b"", "511 bytes": b"a" * 511,
                       "512 bytes": b"b" * 512, "513 bytes": b"c" * 513,
                       "café bytes": b"d"}
            entries = [
                {"lot": "media", "path": "Empty directory", "kind": "dir"},
                *[file_entry(repo, "media", name, content) for name, content in samples.items()],
                file_entry(repo, "media", "repeat", samples["513 bytes"]),
            ]
            entries.sort(key=lambda entry: entry["path"])
            target, manifest, record = publish_fixture(repo, entries)
            with server(repo, f"{legacy}=tools") as port:
                crc = zlib.crc32(b"hello")
                self.assertEqual(request(port, "LIST"), (f"hello 5 {crc:08x} tools\n".encode(), b".\n"))
                self.assertEqual(request(port, "GET hello"), (f"OK 5 {crc:08x}\n".encode(), b"hello"))
                self.assertEqual(request(port, "CATALOG"), (b"OK\n", store.canonical(record) + b"\n.\n"))
                self.assertEqual(request(port, f"MANIFEST {target}"),
                                 (f"OK {len(manifest)}\n".encode(), manifest))
                self.assertTrue(request(port, "MANIFEST " + "../" * 21)[0].startswith(b"NO "))
                raw = archive_payload(*request(port, f"ARCHIVE {target}"))
                self.assertEqual(gzip.decompress(raw)[-1024:], bytes(1024))
                with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as archive:
                    for name, expected in dict(samples, repeat=samples["513 bytes"]).items():
                        self.assertEqual(archive.extractfile(f"media/{name}").read(), expected)
                    self.assertTrue(archive.getmember("media/Empty directory").isdir())
                    self.assertEqual(len(archive.getmembers()), len(entries))
                selection = [0, 3]
                encoded = store.canonical(selection)
                raw = archive_payload(*request(port, f"ARCHIVE {target} {len(encoded)}", encoded))
                with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as archive:
                    self.assertEqual(archive.getnames(), ["media/" + entries[i]["path"] for i in selection])
                for selection in ([3, 0], [0, 0], [-1], [True], [len(entries)]):
                    encoded = store.canonical(selection)
                    self.assertTrue(request(port, f"ARCHIVE {target} {len(encoded)}", encoded)[0].startswith(b"NO "))
                self.assertTrue(request(port, f"ARCHIVE {target} 100", b"[]")[0].startswith(b"NO "))
                self.assertTrue(request(port, f"ARCHIVE {target} {store.MAX_SELECTION + 1}")[0].startswith(b"NO "))

    def test_corrupt_truncated_and_missing_objects(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = repository(Path(temp) / "repo")
            data = b"correct"
            entry = file_entry(repo, "media", "sample", data)
            target, _, _ = publish_fixture(repo, [entry])
            obj = store.object_path(repo, entry["sha256"])
            encoded = obj.read_bytes()
            with server(repo) as port:
                self.assertTrue(request(port, "GET porter_store.py")[0].startswith(b"NO "))
                for broken in (
                    encoded[:-2],
                    encoded[:-1] + bytes([encoded[-1] ^ 1]),
                    encoded + store.gzip_member(b""),
                    store.gzip_member(b"X" * len(data) + bytes(store.padding(len(data)))),
                    store.gzip_member(data + b"X" * store.padding(len(data))),
                ):
                    obj.write_bytes(broken)
                    header, partial = request(port, f"ARCHIVE {target}")
                    self.assertLess(len(partial), int(header.split()[1]))
                obj.unlink()
                self.assertTrue(request(port, f"ARCHIVE {target}")[0].startswith(b"NO "))
                self.assertEqual(request(port, "CATALOG")[0], b"OK\n")


class FileSnapshotTests(unittest.TestCase):
    def test_windows_path_and_handle_timestamps_use_separate_snapshots(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "object.gz"
            path.write_bytes(b"contents")
            named = path.stat()
            fields = {field: getattr(named, field) for field in (
                "st_dev", "st_ino", "st_mode", "st_size", "st_mtime_ns", "st_ctime_ns"
            )}
            opened = SimpleNamespace(**dict(fields, st_ctime_ns=fields["st_ctime_ns"] + 1))
            windows = dict(vars(os), name="nt", fstat=lambda fd: SimpleNamespace(**vars(opened)))
            with patch.object(store, "os", SimpleNamespace(**windows)):
                with store.regular_stream(path, named) as handle:
                    self.assertEqual(handle.read(), b"contents")
                with self.assertRaisesRegex(store.PorterError, "changed while reading"):
                    with store.regular_stream(path, named):
                        opened.st_ctime_ns += 1
                opened.st_ino += 1
                with self.assertRaisesRegex(store.PorterError, "changed while opening"):
                    with store.regular_stream(path, named):
                        self.fail("opened an object with a different identity")


@unittest.skipIf(os.name == "nt", "Make publisher runs on Linux")
class PublisherTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="porter-tests-")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.checkout = self.root / "checkout A"
        self.checkout.mkdir()
        (self.checkout / "tools").mkdir()
        shutil.copyfile(CHECKOUT / "GNUmakefile", self.checkout / "GNUmakefile")
        for name in ("porter_store.py", "porter_publish.py"):
            shutil.copyfile(TOOLS / name, self.checkout / "tools" / name)
        (self.checkout / ".gitignore").write_text("disk/\nporter-sources.conf\n", encoding="utf-8")
        self.run_git("init", "-q", "-b", "preview")
        self.run_git("add", ".")
        self.run_git("-c", "user.name=Porter test", "-c", "user.email=porter@invalid", "commit", "-qm", "fixture")
        self.repo = self.root / "publication repo"
        self.env = dict(os.environ, NAME="ambient label")
        self.sources = self.checkout / "porter-sources.conf"

    def run_git(self, *args):
        subprocess.run(["git", *args], cwd=self.checkout, check=True, capture_output=True)

    def args(self, *options):
        return ["make", "package", f"PORTER_REPO={self.repo}", *options]

    def package(self, *options, success=True):
        result = subprocess.run(self.args(*options), cwd=self.checkout, env=self.env,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def records(self):
        return [json.loads(record) for record in store.catalog(self.repo)]

    def load(self, digest):
        return store.load_manifest(self.repo, digest)[1]

    def test_concurrency_dedup_removal_empty_nested_and_labels(self):
        media = self.root / "media root"
        media.mkdir()
        (media / "empty").mkdir()
        (media / "clip 1").write_bytes(b"same bytes")
        (media / "clip 2").write_bytes(b"same bytes")
        (media / "excluded link").symlink_to(media / "clip 1")
        nested = media / "separate"
        nested.mkdir()
        (nested / "solo").write_bytes(b"other bytes")
        self.sources.write_text(f"[sources media]\nmedia += {media}\n[sources sound]\nsounds += {nested}\n")
        other = self.root / "checkout B"
        shutil.copytree(self.checkout, other)
        options = self.args("SCOPES=media,sound", "NAME=Repeated title", "BASELINE=1")
        with (subprocess.Popen(options, cwd=self.checkout, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as first,
              subprocess.Popen(options, cwd=other, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as second):
            for proc in (first, second):
                self.assertIn(b"excluded link", (output := proc.communicate(timeout=30))[1])
                self.assertEqual(proc.returncode, 0, output)
        first, second = self.records()
        self.assertNotEqual(first["id"], second["id"])
        self.assertEqual(first["targets"], second["targets"])
        self.assertEqual(first["name"], "Repeated title")
        self.assertTrue(first["baseline"])
        self.assertNotIn(str(self.root), json.dumps(first))
        self.assertEqual(len(list((self.repo / "objects").glob("*.gz"))), 2)
        media_id = first["targets"]["media"]
        self.assertEqual([entry["path"] for entry in self.load(media_id)["entries"]], ["clip 1", "clip 2", "empty"])
        self.assertEqual([entry["path"] for entry in self.load(first["targets"]["sound"])["entries"]], ["solo"])

        for path in (media / "clip 1", media / "clip 2", media / "excluded link"):
            path.unlink()
        (media / "empty").rmdir()
        self.package("SCOPES=media")
        last = next(row for row in self.records() if row["name"].startswith("preview "))
        self.assertIn("T", last["published_at"])
        self.assertFalse(last["baseline"])
        self.assertEqual(self.load(last["targets"]["media"])["entries"], [])
        self.assertEqual(self.load(last["targets"]["media"])["lots"], ["media"])
        self.assertEqual(len(self.load(media_id)["entries"]), 3)
        self.assertEqual(len(list((self.repo / "objects").glob("*.gz"))), 2)
        shutil.rmtree(media)
        with server(self.repo) as port:
            data = archive_payload(*request(port, f"ARCHIVE {media_id}"))
            with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
                self.assertEqual(archive.extractfile("media/clip 1").read(), b"same bytes")

    def test_collision_missing_source_dry_run_and_path_boundary(self):
        roots = [self.root / first for first in ("first root", "second root")]
        for root in roots:
            root.mkdir()
        sources = "[sources media]\n" + "".join(f"tape += {root}\n" for root in roots)
        self.sources.write_text(sources)
        for root, data in zip(roots, (b"one", b"two")):
            (root / "same").write_bytes(data)
        result = self.package("SCOPES=media", success=False)
        self.assertIn("conflicting tape/same", result)
        for root in roots:
            self.assertIn(str(root), result)
        self.assertEqual(self.records(), [])
        shutil.rmtree(roots[1])
        self.assertIn(str(roots[1]), self.package("SCOPES=media", success=False))
        self.assertEqual(self.records(), [])
        self.package("SCOPES=media", "-n")
        self.assertEqual(self.records(), [])

        roots[1].mkdir()
        (roots[1] / "same").write_bytes(b"one")
        parent = roots[0] / ("a" * 70) / ("b" * 79)
        parent.mkdir(parents=True)
        long_file = parent / ("c" * 99)
        long_file.write_bytes(b"one")
        self.package("SCOPES=media", "NAME=Path boundary")
        entries = self.load(self.records()[0]["targets"]["media"])["entries"]
        filenames = [e for e in entries if e["kind"] == "file"]
        self.assertEqual(len(filenames), 2)
        self.assertEqual(max(len("tape/" + e["path"]) for e in filenames), 255)

    def test_changed_source_and_corrupt_reuse(self):
        objdir = repository(self.repo)
        source = self.root / "movie"
        source.write_bytes(b"first")
        item = publisher.Input("media", "tape", "movie", source, source.stat())
        read = publisher.read_source

        def change_after_hash(current, member=None):
            digest = read(current, member)
            if member is None:
                source.write_bytes(b"second")
            return digest

        with patch.object(publisher, "read_source", side_effect=change_after_hash):
            with self.assertRaisesRegex(store.PorterError, "file changed"):
                publisher.capture(objdir, item, {})
        self.assertEqual(list((objdir / "objects").iterdir()), [])
        item = publisher.Input("media", "tape", "movie", source, source.stat())
        digest = publisher.capture(objdir, item, {})
        path = store.object_path(objdir, digest)
        with patch.object(publisher.gzip, "GzipFile", side_effect=AssertionError("unexpected recompression")):
            self.assertEqual(publisher.capture(objdir, item, {}), digest)
        path.write_bytes(b"corrupt")
        with self.assertRaisesRegex(store.PorterError, "corrupt gzip"):
            publisher.capture(objdir, item, {})
        self.assertEqual(path.read_bytes(), b"corrupt")

    def test_inventory_changes_during_collection(self):
        repo = repository(self.repo)
        assets = self.root / "assets"
        assets.mkdir()
        (assets / "first").write_bytes(b"one")
        self.sources.write_text(f"[sources media]\nmedia += {assets}\n")
        source_files = [(self.sources, True)]
        build = publisher.read_sources(self.checkout, repo, source_files)
        capture = publisher.capture

        def add_file(store_root, item, checked):
            digest = capture(store_root, item, checked)
            (assets / "added during scan").touch()
            return digest

        with patch.object(publisher, "capture", side_effect=add_file):
            with self.assertRaisesRegex(store.PorterError, "changed during publication"):
                publisher.publish_targets(self.checkout, repo, ["media"], [], source_files, build)
        self.assertEqual(self.records(), [])


if __name__ == "__main__":
    unittest.main()
