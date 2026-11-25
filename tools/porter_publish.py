#!/usr/bin/env python3
"""Build the selected scopes and publish them to a local Porter repository."""

from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime
import fcntl
import gzip
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import stat
import subprocess
import sys
import tempfile
from uuid import uuid4

from porter_store import (
    CHUNK, LOT, MAX_ENTRIES, MAX_FILE, MAX_JSON, PorterError, archive_header,
    canonical, check_manifest, check_member, fingerprint, is_link, object_path,
    padding, read_metadata, regular_info, regular_stream,
)


BASE_LOTS = {"apps", "libs", "tests", "boot", "config", "fonts", "frames",
             "pages", "icons", "certs", "licenses"}
SECTION = re.compile(r"\[sources(?: ([a-z][a-z0-9_-]{0,30}))?\]", re.ASCII)


@dataclass(frozen=True)
class Source:
    scope: str
    lot: str
    root: Path


@dataclass(frozen=True)
class Input:
    scope: str
    lot: str
    path: str
    source: Path
    info: os.stat_result


def absolute(root, path):
    return Path(os.path.abspath(root / Path(path).expanduser()))


def read_sources(root, repo, files):
    sources = []
    documents = {}
    owners = {}
    for path, required in files:
        try:
            raw = read_metadata(path)
        except FileNotFoundError:
            if required or path.is_symlink():
                raise
            documents[path] = None
            continue
        documents[path] = raw
        scope = None
        for number, line in enumerate(raw.decode("utf-8").splitlines(), 1):
            line = line.strip()
            if not line or line.startswith(("#", ";")):
                continue
            section = SECTION.fullmatch(line)
            if section:
                scope = section[1] or "system"
                continue
            key, sign, value = line.partition("+=")
            lot, name = key.strip(), value.strip()
            if not scope or not sign or not LOT.fullmatch(lot) or not name:
                raise PorterError(f"{path}:{number}: expected a source section or lot += directory")
            folder = absolute(root, name)
            if folder.is_relative_to(repo) or repo.is_relative_to(folder):
                raise PorterError(f"source and package repository overlap: {folder}")
            origin = f"{path}:{number}"
            record = Source(scope, lot, folder)
            if folder in owners:
                previous, previous_origin = owners[folder]
                if previous != record:
                    raise PorterError(f"conflicting registrations for {folder}: {previous_origin} and {origin}")
                continue
            owners[folder] = record, origin
            sources.append(record)
    return sources, documents


def directory_info(path):
    info = path.lstat()
    if is_link(info) or not stat.S_ISDIR(info.st_mode):
        raise PorterError(f"expected a directory without links: {path}")
    return info


def require_unlinked_parents(path):
    for parent in reversed(path.parents):
        directory_info(parent)


def collect(selected, builtins, sources, report):
    inputs = []
    scanned = {}
    roots = {source.root for source in sources}
    for path, lot, name in builtins:
        require_unlinked_parents(path)
        inputs.append(Input("system", lot, name, path, regular_info(path)))
    for source in sources:
        if source.scope not in selected:
            continue
        require_unlinked_parents(source.root)
        directory_info(source.root)
        queue = [source.root]
        while queue:
            folder = queue.pop()
            scanned[folder] = fingerprint(directory_info(folder))
            with os.scandir(folder) as listing:
                for item in sorted(listing, key=lambda entry: entry.name):
                    child = Path(item.path)
                    if child in roots:
                        continue
                    before = item.stat(follow_symlinks=False)
                    rel = child.relative_to(source.root).as_posix()
                    if is_link(before) or not (stat.S_ISDIR(before.st_mode) or stat.S_ISREG(before.st_mode)):
                        if report:
                            print(f"porter: excluded link or special file: {child}", file=sys.stderr)
                        continue
                    inputs.append(Input(source.scope, source.lot, rel, child, before))
                    if stat.S_ISDIR(before.st_mode):
                        queue.append(child)
    return inputs, scanned


def input_snapshot(inputs, directories):
    return ({(i.scope, i.lot, i.path, i.source): fingerprint(i.info) for i in inputs},
            directories)


def built_in_inputs(root, inventory):
    inputs = []
    for line in inventory.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 3 or not LOT.fullmatch(fields[0]):
            raise PorterError("invalid Make inventory")
        lot, name, src = fields
        inputs.append((absolute(root, src), lot, name))
    return inputs


@contextmanager
def lock(path):
    with path.open("ab") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        yield


def sync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def put_metadata(path, raw):
    if len(raw) > MAX_JSON:
        raise PorterError(f"metadata exceeds {MAX_JSON} bytes")
    if path.exists():
        if read_metadata(path) != raw:
            raise PorterError(f"immutable record differs: {path}")
        return
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".incoming-", delete=False) as out:
        temp = Path(out.name)
        try:
            out.write(raw)
            out.flush()
            os.fsync(out.fileno())
            out.close()
            os.replace(temp, path)
        finally:
            temp.unlink(missing_ok=True)


def read_source(item, member=None):
    size = item.info.st_size
    hasher = hashlib.sha256()
    total = 0
    require_unlinked_parents(item.source)
    with regular_stream(item.source, item.info) as source:
        while chunk := source.read(CHUNK):
            total += len(chunk)
            if total > size:
                raise PorterError(f"file grew while reading: {item.source}")
            hasher.update(chunk)
            if member is not None:
                member.write(chunk)
    if total != size:
        raise PorterError(f"file size changed: {item.source}")
    return hasher.hexdigest()


def capture(repo, item, verified):
    size = item.info.st_size
    if size > MAX_FILE:
        raise PorterError(f"file is too large for ustar: {item.source}")
    digest = read_source(item)
    if digest in verified:
        if verified[digest] != size:
            raise PorterError("conflicting object sizes")
        return digest

    dest = object_path(repo, digest)
    if dest.exists():
        with regular_stream(dest) as existing:
            check_member(existing, digest, size)
    else:
        with tempfile.NamedTemporaryFile(dir=dest.parent, prefix=".incoming-", delete=False) as out:
            temp = Path(out.name)
            try:
                with gzip.GzipFile(filename="", fileobj=out, mode="wb", mtime=0, compresslevel=6) as member:
                    if read_source(item, member) != digest:
                        raise PorterError(f"file contents changed: {item.source}")
                    member.write(bytes(padding(size)))
                out.flush()
                os.fsync(out.fileno())
                out.close()
                with regular_stream(temp) as check:
                    check_member(check, digest, size)
                os.replace(temp, dest)
            finally:
                temp.unlink(missing_ok=True)
    verified[digest] = size
    return digest


def add_entry(entries, entry, source):
    key = entry["lot"], entry["path"]
    if key in entries:
        previous, previous_source = entries[key]
        if previous != entry:
            raise PorterError(f"conflicting {key[0]}/{key[1]}: {previous_source} and {source}")
        return
    if len(entries) >= MAX_ENTRIES:
        raise PorterError(f"too many entries in scope (maximum {MAX_ENTRIES})")
    archive_header(entry)
    entries[key] = entry, source


def manifests(repo, selected, inputs, sources):
    records = {scope: {} for scope in selected}
    lots = {scope: set() for scope in selected}
    if "system" in selected:
        lots["system"].update(BASE_LOTS)
    for source in sources:
        if source.scope in selected:
            lots[source.scope].add(source.lot)
    verified = {}
    captured = {}
    for item in inputs:
        entries = records[item.scope]
        lots[item.scope].add(item.lot)
        entry = {"lot": item.lot, "path": item.path, "kind": "dir"}
        if stat.S_ISREG(item.info.st_mode):
            entry.update(kind="file", size=item.info.st_size)
        # Validate the original name before PurePosixPath can normalize it.
        archive_header(entry)
        for parent in reversed(PurePosixPath(item.path).parents):
            if parent != PurePosixPath("."):
                add_entry(entries, {"lot": item.lot, "path": parent.as_posix(), "kind": "dir"},
                          item.source)
        if entry["kind"] == "file":
            if item.source not in captured:
                captured[item.source] = capture(repo, item, verified)
            entry["sha256"] = captured[item.source]
        add_entry(entries, entry, item.source)
    for scope, entries in records.items():
        kernel = None
        if scope == "system":
            kernel_entry = entries.get(("boot", "os64_kernel"))
            if kernel_entry is None or kernel_entry[0]["kind"] != "file":
                raise PorterError("Make inventory is missing the system kernel")
            kernel = {"elf_sha256": kernel_entry[0]["sha256"]}
        manifest = {"version": 1, "scope": scope, "kernel": kernel, "lots": sorted(lots[scope]),
                    "entries": [entries[key][0] for key in sorted(entries)]}
        check_manifest(manifest)
        yield scope, manifest


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def provenance(root):
    revision = git(root, "rev-parse", "HEAD")
    branch = git(root, "rev-parse", "--abbrev-ref", "HEAD")
    dirty = git(root, "status", "--porcelain", "--untracked-files=normal")
    return revision, branch, dirty


def toolchain():
    defaults = {"cc": "x86_64-elf-gcc", "ld": "x86_64-elf-ld",
                "objcopy": "x86_64-elf-objcopy", "nasm": "nasm"}
    result = {}
    for key, default in defaults.items():
        command = shlex.split(os.environ.get(f"PORTER_TOOL_{key.upper()}") or default)
        if key == "cc" and len(command) > 1 and Path(command[0]).name == "ccache":
            command = command[1:]
        result[key] = subprocess.check_output(command + ["--version"], text=True).splitlines()[0]
    return result


def publish_targets(root, repo, selected, builtins, source_files, build):
    inputs, folders = collect(selected, builtins, build[0], True)
    snapshot = input_snapshot(inputs, folders)
    targets = {}
    for scope, manifest in manifests(repo, selected, inputs, build[0]):
        raw = canonical(manifest)
        digest = hashlib.sha256(raw).hexdigest()
        put_metadata(repo / "manifests" / (digest + ".json"), raw)
        targets[scope] = digest
    sync_dir(repo / "objects")
    sync_dir(repo / "manifests")
    if (build != read_sources(root, repo, source_files)
            or snapshot != input_snapshot(*collect(selected, builtins, build[0], False))):
        raise PorterError("source selection or contents changed during publication")
    return targets


def main():
    # Make runs recursive recipes even when it is only printing, querying or
    # touching targets. These modes must not publish or start a sub-build.
    flags = os.environ.get("MAKEFLAGS", "").split()
    if flags and not flags[0].startswith("-") and set(flags[0]).intersection("nqt"):
        return
    root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel"))
    common = Path(git(root, "rev-parse", "--path-format=absolute", "--git-common-dir")).parent
    default_repo = common.with_name(common.name + "-porter")
    repo = absolute(root, os.environ.get("PORTER_REPO") or default_repo).resolve()
    if repo.is_relative_to(root) or repo.is_relative_to(common):
        raise PorterError("the package repository must be outside the checkout")
    selected = os.environ.get("PORTER_SCOPES", "system").split(",")
    if any(not LOT.fullmatch(scope) for scope in selected) or len(set(selected)) != len(selected):
        raise PorterError("SCOPES must be a comma-separated list of distinct scope names")
    baseline = os.environ.get("PORTER_BASELINE", "")
    if baseline not in ("", "0", "1"):
        raise PorterError("BASELINE must be 0 or 1")
    name = os.environ.get("PORTER_LABEL", "")
    if len(name.encode("utf-8")) > 256 or any(ord(ch) < 32 or ord(ch) == 127 for ch in name):
        raise PorterError("NAME must be at most 256 UTF-8 bytes with no control characters")
    source_files = [(absolute(root, os.environ.get("PORTER_SOURCES") or "porter-sources.conf"),
                     bool(os.environ.get("PORTER_SOURCES")))]
    overlay = os.environ.get("PORTER_SOURCES_LOCAL")
    if overlay:
        source_files.append((absolute(root, overlay), True))

    for directory in ("objects", "manifests", "publications"):
        path = repo / directory
        path.mkdir(parents=True, exist_ok=True)
        directory_info(path)
    sync_dir(repo)
    local = root / "disk"
    local.mkdir(exist_ok=True)
    with lock(local / ".porter-build.lock"), tempfile.TemporaryDirectory() as scratch:
        inventory = Path(scratch) / "inventory"
        build = read_sources(root, repo, source_files)
        for scope in selected:
            if scope != "system" and not any(source.scope == scope for source in build[0]):
                raise PorterError(f"no sources configured for scope {scope!r}")
        before = provenance(root)
        builtins = []
        tools = {}
        if "system" in selected:
            environment = dict(os.environ, PORTER_INVENTORY=str(inventory))
            subprocess.run([sys.argv[1], "porter-package-inputs"], cwd=root,
                           env=environment, check=True, close_fds=False)
            builtins = built_in_inputs(root, inventory)
            tools = toolchain()
        with lock(repo / ".publish.lock"):
            targets = publish_targets(root, repo, selected, builtins, source_files, build)
            if provenance(root) != before:
                raise PorterError("checkout changed during publication")
            revision, branch, dirty = before
            now = datetime.now().astimezone().isoformat(timespec="seconds")
            identifier = uuid4().hex
            label = revision[:12] if branch == "HEAD" else branch
            # Host paths stay private. The rule summary describes how the
            # inputs were selected without exporting the source configs.
            record = {
                "version": 1, "id": identifier, "name": name or f"{label} {now}",
                "baseline": baseline == "1", "published_at": now,
                "revision": revision, "branch": branch, "dirty": bool(dirty), "toolchain": tools,
                "input_rules": {"make": "system" in selected, "recursive": True,
                                "nested_sources_own_subtrees": True,
                                "sources": [
                                    {"scope": source.scope, "lot": source.lot,
                                     "location": "checkout" if source.root.is_relative_to(root) else "external"}
                                    for source in build[0] if source.scope in selected
                                ]},
                "targets": targets,
            }
            put_metadata(repo / "publications" / (identifier + ".json"), canonical(record))
            sync_dir(repo / "publications")
    print(f"porter: published {identifier} " + " ".join(f"{s}={h}" for s, h in targets.items()))


if __name__ == "__main__":
    try:
        main()
    except (PorterError, OSError, UnicodeError, subprocess.CalledProcessError) as error:
        sys.exit(f"porter: {error}")
