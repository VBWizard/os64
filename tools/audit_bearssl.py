#!/usr/bin/env python3
"""Link the complete foundation archive and audit its freestanding boundary."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
USERLAND = ROOT / "userland"
OBJ = USERLAND / "obj"
ARCHIVE = OBJ / "libbearssl-foundation.a"
INPUTS = OBJ / "pic/libtls/port/platform_inputs.c.o"


def command(*args):
    return subprocess.check_output([str(a) for a in args], text=True)


subprocess.run(["python3", str(ROOT / "tools/check_bearssl_import.py")], check=True)
subprocess.run(["make", "-C", str(USERLAND), str(ARCHIVE),
                str(USERLAND / "bin/libos64.so"), str(USERLAND / "bin/tests/bearssltest"),
                str(USERLAND / "bin/tests/tlsinputtest")], check=True)
merged = OBJ / "bearssl-complete.o"
command("x86_64-elf-ld", "-r", "--whole-archive", ARCHIVE, "--no-whole-archive", "-o", merged)
symbols = command("x86_64-elf-nm", "-u", merged)
undefined = {line.split()[-1] for line in symbols.splitlines()}
expected = {"_GLOBAL_OFFSET_TABLE_", "os64_malloc", "os64_free",
            "os64_memcpy", "os64_memmove", "os64_memset", "os64_strlen"}
if undefined != expected:
    raise SystemExit("unexpected complete-core dependencies: " + repr(undefined))
shared = OBJ / "bearssl-link-audit.so"
command("x86_64-elf-ld", "-shared", "--no-undefined", "--hash-style=sysv",
        "-z", "text", "-z", "noexecstack", "--whole-archive", ARCHIVE,
        "--no-whole-archive", USERLAND / "bin/libos64.so", "-o", shared,
        "-Map=" + str(OBJ / "bearssl-link-audit.map"))
input_imports = {line.split()[-1] for line in command("x86_64-elf-nm", "-u", INPUTS).splitlines()}
if input_imports - {"_GLOBAL_OFFSET_TABLE_"} != {
        "os64_open", "os64_read", "os64_close", "os64_time",
        "os64_tls_engine_create", "os64_tls_policy_factory"}:
    raise SystemExit("unexpected OS-input dependencies: " + repr(input_imports))
input_shared = OBJ / "tls-inputs-link-audit.so"
command("x86_64-elf-ld", "-shared", "--no-undefined", "--hash-style=sysv",
        "-z", "text", "-z", "noexecstack", INPUTS, "--whole-archive", ARCHIVE,
        "--no-whole-archive", USERLAND / "bin/libos64.so", "-o", input_shared)
for binary in (shared, input_shared, USERLAND / "bin/tests/bearssltest", USERLAND / "bin/tests/tlsinputtest"):
    dynamic = command("x86_64-elf-readelf", "-dW", binary)
    if re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic) != ["libos64.so"]:
        raise SystemExit("unexpected DT_NEEDED: " + str(binary))
    if re.search(r"\((TEXTREL|INIT|FINI|INIT_ARRAY|FINI_ARRAY)\)", dynamic):
        raise SystemExit("unexpected dynamic runtime machinery: " + str(binary))
    relocations = command("x86_64-elf-readelf", "-rW", binary)
    kinds = set(re.findall(r"R_X86_64_\w+", relocations))
    if not kinds <= {"R_X86_64_RELATIVE", "R_X86_64_JUMP_SLOT", "R_X86_64_GLOB_DAT"}:
        raise SystemExit("unexpected relocation kinds: " + repr(kinds))
    exports = command("x86_64-elf-nm", "-D", "--defined-only", binary)
    if re.search(r"\b(br_\w+|os64_bearssl_\w+|memcpy|memmove|memset|memcmp|strlen)$", exports, re.M):
        raise SystemExit("private symbol exported: " + exports)
    if binary in (shared, input_shared) and exports.strip():
        raise SystemExit("private audit DSO unexpectedly exports symbols")
assembly = command("x86_64-elf-objdump", "-d", merged, INPUTS)
if re.search(r"\t(?:rdrand|rdseed|syscall|sysenter)\b", assembly):
    raise SystemExit("unexpected hardware RNG or syscall instruction in core")
license_text = (ROOT / "userland/libtls/upstream/LICENSE.txt").read_bytes()
for name in ("bearssltest", "tlsinputtest"):
    if license_text not in (USERLAND / "bin/tests" / name).read_bytes():
        raise SystemExit(name + " does not retain the upstream binary-distribution license")
print("BearSSL ELF audit PASS: six os64 imports, hidden core/client, supported relocations, retained license")
print("TLS input ELF audit PASS: four OS service imports, hidden adapter, guest fixture linked")
print("Audit artifacts: " + str(shared) + " and " + str(OBJ / "bearssl-link-audit.map"))
