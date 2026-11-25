"""Porter v1 repository formats and streaming archive service."""

from contextlib import contextmanager
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import tarfile
import zlib


CHUNK = 64 * 1024
BLOCK = 512
MAX_FILE = 8**11 - 1  # ustar's eleven octal size digits
MAX_ENTRIES = 16_384
MAX_JSON = 16 * 1024 * 1024
MAX_SELECTION = 128 * 1024
ID = re.compile(r"[0-9a-f]{64}", re.ASCII)
LOT = re.compile(r"[a-z][a-z0-9_-]{0,30}", re.ASCII)
PUBLICATION_ID = re.compile(r"[0-9a-f]{32}", re.ASCII)


class PorterError(ValueError):
    pass


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("ascii")


def parse_json(raw):
    try:
        value = json.loads(raw)
        if canonical(value) != raw:
            raise PorterError("JSON is not canonical")
        return value
    except (ValueError, RecursionError, UnicodeError) as exc:
        raise PorterError("invalid canonical JSON") from exc


def is_link(info):
    return stat.S_ISLNK(info.st_mode) or bool(
        getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    )


def fingerprint(info):
    return info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def regular_info(path):
    info = path.lstat()
    if is_link(info) or not stat.S_ISREG(info.st_mode):
        raise PorterError(f"expected a regular file: {path}")
    return info


def open_matches_path(named, opened):
    if os.name == "nt":
        # Windows path and handle stat can report different ctime for the
        # same file. Compare metadata for changes within each stat API.
        return (os.path.samestat(named, opened)
                and stat.S_IFMT(named.st_mode) == stat.S_IFMT(opened.st_mode)
                and named.st_size == opened.st_size)
    return fingerprint(named) == fingerprint(opened)


@contextmanager
def regular_stream(path, expected=None):
    before = regular_info(path)
    if expected is not None and fingerprint(before) != fingerprint(expected):
        raise PorterError(f"file changed: {path}")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    flags |= getattr(os, "O_NONBLOCK", 0)
    with os.fdopen(os.open(path, flags), "rb") as stream:
        opened = os.fstat(stream.fileno())
        if is_link(opened) or not open_matches_path(before, opened):
            raise PorterError(f"file changed while opening: {path}")
        yield stream
        if (fingerprint(os.fstat(stream.fileno())) != fingerprint(opened)
                or fingerprint(regular_info(path)) != fingerprint(before)):
            raise PorterError(f"file changed while reading: {path}")


def read_metadata(path):
    with regular_stream(path) as stream:
        data = stream.read(MAX_JSON + 1)
    if len(data) > MAX_JSON:
        raise PorterError(f"metadata exceeds {MAX_JSON} bytes: {path}")
    return data


def object_path(repo, digest):
    if not isinstance(digest, str) or not ID.fullmatch(digest):
        raise PorterError("invalid content ID")
    return repo / "objects" / (digest + ".gz")


def gzip_member(data):
    output = io.BytesIO()
    with gzip.GzipFile(filename="", fileobj=output, mode="wb", mtime=0) as member:
        member.write(data)
    return output.getvalue()


def padding(size):
    return (-size) % BLOCK


def archive_header(entry):
    name = f'{entry["lot"]}/{entry["path"]}'
    relative = entry["path"]
    if (any(part in ("", ".", "..") for part in relative.split("/"))
            or any(ord(ch) < 32 or ord(ch) == 127 or ch in "\\:" for ch in relative)):
        raise PorterError(f"unsafe archive path: {name!r}")
    item = tarfile.TarInfo(name)
    if entry["kind"] == "dir":
        item.type = tarfile.DIRTYPE
        item.mode = 0o755
    else:
        item.size = entry["size"]
        item.mode = 0o755 if entry["lot"] in ("apps", "tests") and "/" not in relative else 0o644
    try:
        # The directory slash added by TarInfo also has to fit Os64's buffer.
        if len(name.encode("utf-8")) + (item.type == tarfile.DIRTYPE) >= 256:
            raise ValueError("Os64 tar path limit")
        return item.tobuf(tarfile.USTAR_FORMAT, "utf-8", "strict")
    except (ValueError, UnicodeError) as exc:
        raise PorterError(f"path or size is outside Os64 ustar limits: {name!r}") from exc


def check_manifest(manifest):
    if (not isinstance(manifest, dict)
            or set(manifest) != {"version", "scope", "kernel", "lots", "entries"}
            or type(manifest["version"]) is not int or manifest["version"] != 1):
        raise PorterError("invalid manifest format")
    scope, lots, entries = manifest["scope"], manifest["lots"], manifest["entries"]
    if (not isinstance(scope, str) or not LOT.fullmatch(scope)
            or not isinstance(lots, list) or not lots
            or any(not isinstance(lot, str) or not LOT.fullmatch(lot) for lot in lots)
            or lots != sorted(set(lots))
            or not isinstance(entries, list) or len(entries) > MAX_ENTRIES):
        raise PorterError("invalid manifest scope, lots or entry count")
    kernel = manifest["kernel"]
    if scope == "system":
        if (not isinstance(kernel, dict) or set(kernel) != {"elf_sha256"}
                or not isinstance(kernel["elf_sha256"], str) or not ID.fullmatch(kernel["elf_sha256"])):
            raise PorterError("invalid system kernel contract")
    elif kernel is not None:
        raise PorterError("independent scopes cannot declare a kernel")
    known = {}
    sizes = {}
    previous = ("", "")
    for entry in entries:
        if not isinstance(entry, dict) or not {"lot", "path", "kind"} <= entry.keys():
            raise PorterError("invalid manifest entry")
        lot, path, kind = entry["lot"], entry["path"], entry["kind"]
        if not isinstance(lot, str) or lot not in lots or not isinstance(path, str):
            raise PorterError("invalid manifest path or lot")
        if (lot, path) <= previous:
            raise PorterError("manifest entries are duplicated or unsorted")
        previous = (lot, path)
        parent, sep, _ = path.rpartition("/")
        if sep and known.get((lot, parent)) != "dir":
            raise PorterError(f"missing directory for {lot}/{path}")
        if kind == "file":
            if (set(entry) != {"lot", "path", "kind", "size", "sha256"}
                    or type(entry["size"]) is not int or not 0 <= entry["size"] <= MAX_FILE
                    or not isinstance(entry["sha256"], str) or not ID.fullmatch(entry["sha256"])):
                raise PorterError("invalid manifest file")
            if sizes.setdefault(entry["sha256"], entry["size"]) != entry["size"]:
                raise PorterError("conflicting content lengths")
        elif kind != "dir" or set(entry) != {"lot", "path", "kind"}:
            raise PorterError("invalid manifest directory")
        archive_header(entry)
        known[(lot, path)] = kind
    if scope == "system" and not any(
        e["lot"] == "boot" and e["path"] == "os64_kernel" and e.get("sha256") == kernel["elf_sha256"]
        for e in entries
    ):
        raise PorterError("system manifest does not contain its kernel")


def load_manifest(repo, digest):
    if not isinstance(digest, str) or not ID.fullmatch(digest):
        raise PorterError("invalid target ID")
    raw = read_metadata(repo / "manifests" / (digest + ".json"))
    if hashlib.sha256(raw).hexdigest() != digest:
        raise PorterError("manifest digest mismatch")
    value = parse_json(raw)
    check_manifest(value)
    return raw, value


def check_member(stream, digest, size, send=None, wire_size=None):
    """Verify one gzip member, including its zero tar padding and trailer."""
    decoder = zlib.decompressobj(31)
    hasher = hashlib.sha256()
    decoded = compressed = 0
    padded = size + padding(size)
    while True:
        chunk = stream.read(CHUNK)
        if not chunk:
            break
        compressed += len(chunk)
        if decoder.eof or (wire_size is not None and compressed > wire_size):
            raise PorterError("unexpected bytes after gzip member")
        pending = chunk
        while True:
            try:
                data = decoder.decompress(pending, CHUNK)
            except zlib.error as exc:
                raise PorterError("corrupt gzip member") from exc
            file_part = min(len(data), max(0, size - decoded))
            hasher.update(data[:file_part])
            decoded += len(data)
            if decoded > padded or any(data[file_part:]) or decoder.unused_data:
                raise PorterError("invalid gzip member length or tar padding")
            pending = decoder.unconsumed_tail
            if decoder.eof or (not pending and len(data) < CHUNK):
                break
        if send is not None:
            send(chunk)
    if (not decoder.eof or decoded != padded or hasher.hexdigest() != digest
            or (wire_size is not None and compressed != wire_size)):
        raise PorterError("truncated gzip member or object digest mismatch")


def stream_archive(repo, entries, reply):
    headers = []
    objects = {}
    end = gzip_member(bytes(2 * BLOCK))
    length = len(end)
    for entry in entries:
        header = gzip_member(archive_header(entry))
        headers.append(header)
        length += len(header)
        if entry["kind"] == "file":
            digest = entry["sha256"]
            if digest not in objects:
                path = object_path(repo, digest)
                objects[digest] = (path, regular_info(path))
            length += objects[digest][1].st_size

    reply(f"OK {length}\n".encode("ascii"))
    for header, entry in zip(headers, entries):
        reply(header)
        if entry["kind"] == "file":
            digest = entry["sha256"]
            path, info = objects[digest]
            with regular_stream(path, info) as stream:
                check_member(stream, digest, entry["size"], reply, info.st_size)
    reply(end)


def catalog(repo):
    for path in (repo / "publications").iterdir():
        if path.suffix != ".json" or not PUBLICATION_ID.fullmatch(path.stem):
            continue
        raw = read_metadata(path)
        record = parse_json(raw)
        if not isinstance(record, dict) or record.get("id") != path.stem:
            raise PorterError("invalid publication record")
        yield raw + b"\n"


def serve_porter(repo, line, recv, send):
    """Return false for the legacy protocol. A started response ends at EOF."""
    fields = line.split()
    if not fields or fields[0] not in ("CATALOG", "MANIFEST", "ARCHIVE"):
        return False
    if repo is None:
        send(b"NO Porter repository is disabled\n")
        return True
    started = False

    def reply(data):
        nonlocal started
        started = True
        send(data)

    try:
        if fields == ["CATALOG"]:
            reply(b"OK\n")
            for row in catalog(repo):
                reply(row)
            reply(b".\n")
        elif fields[0] == "MANIFEST" and len(fields) == 2:
            raw, _ = load_manifest(repo, fields[1])
            reply(f"OK {len(raw)}\n".encode("ascii"))
            reply(raw)
        elif fields[0] == "ARCHIVE" and len(fields) in (2, 3):
            _, manifest = load_manifest(repo, fields[1])
            entries = manifest["entries"]
            if len(fields) == 3:
                if not re.fullmatch(r"[0-9]{1,6}", fields[2], re.ASCII):
                    raise PorterError("invalid selection length")
                remaining = int(fields[2])
                if not 0 < remaining <= MAX_SELECTION:
                    raise PorterError("invalid selection length")
                request = bytearray()
                while remaining:
                    part = recv(min(CHUNK, remaining))
                    if not part:
                        raise PorterError("truncated selection")
                    request.extend(part)
                    remaining -= len(part)
                selected = parse_json(bytes(request))
                if (not isinstance(selected, list) or len(selected) > len(entries)
                        or any(type(i) is not int or not 0 <= i < len(entries) for i in selected)
                        or selected != sorted(set(selected))):
                    raise PorterError("invalid entry selection")
                entries = [entries[index] for index in selected]
            stream_archive(repo, entries, reply)
        else:
            raise PorterError("invalid Porter request")
    except (PorterError, OSError) as exc:
        # A partial archive must end below its advertised length and without
        # the tar terminator. The diagnostic stays on the serving host.
        print(f"os64serve: Porter request failed: {exc}")
        if not started:
            send(b"NO invalid request or unavailable package\n")
    return True
