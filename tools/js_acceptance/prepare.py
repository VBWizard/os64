#!/usr/bin/env python3
"""Verify retained upstream bytes and emit separate test definitions/drivers."""
import hashlib
import json
from pathlib import Path
import re
import sys

source = Path(__file__).resolve().parent / "upstream"
manifest = json.loads((source / "manifest.json").read_text())


def verify(path, expected):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit(f"upstream hash mismatch: {path}")
    return data


verify(source / "LICENSE", manifest["license_sha256"])
definitions, cases = [], []
for index, entry in enumerate(manifest["files"]):
    data = verify(source / entry["path"], entry["sha256"])
    offset = entry["driver_offset"]
    # Reject an inventory that silently drops a driver call or changes its order.
    driver = "".join(case["name"] + "();" for case in entry["cases"])
    if re.sub(rb"\s", b"", data[offset:]).decode() != driver:
        raise SystemExit(f"upstream driver mismatch: {entry['path']}")
    prefix = data[:offset].decode("utf-8")
    definitions.append(f"static const char upstream_{index}[] =\n" +
                       "\n".join(json.dumps(line, ensure_ascii=False)
                                 for line in prefix.splitlines(keepends=True)) + ";\n")
    for case in entry["cases"]:
        label = entry["path"] + ":" + case["name"]
        call = case["name"] + "();"
        skip = json.dumps(case["skip"]) if "skip" in case else "NULL"
        cases.append(f"{{{json.dumps(label)}, upstream_{index}, {json.dumps(call)}, {skip}}},")
output = "/* Generated from hash-checked upstream fixtures by prepare.py. */\n"
output += "\n".join(definitions)
output += "static const struct language_case {\n"
output += "    const char *name, *definitions, *call, *skip;\n"
output += "} language_cases[] = {\n" + "\n".join(cases) + "\n};\n"
Path(sys.argv[1]).write_text(output)
