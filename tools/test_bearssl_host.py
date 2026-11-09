#!/usr/bin/env python3
"""Run upstream vectors and compare pristine/ported builds under ASan/UBSan."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from check_bearssl_import import BASE, ROOT, pristine

# Explicit names: upstream silently ignores an unknown test name, so validate
# these against its table before accepting a successful process exit.
TESTS = "SHA256 SHA384 SHA512 HMAC HKDF HMAC_DRBG AESCTR_DRBG PRF AES_ct64 AES_CTRCBC_ct64 ChaCha20_ct Poly1305_ctmul RSA_i31 GHASH_ctmul64 GCM EC_prime_i31 EC_p256_m31 EC_c25519_m31 ECDSA_i31".split()


def run(argv, log, cwd=None):
    with log.open("w") as output:
        result = subprocess.run([str(a) for a in argv], stdout=output,
                                stderr=subprocess.STDOUT, cwd=cwd, timeout=600)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}); {log}\n" + log.read_text()[-5000:])


def check(work):
    ref = work / "pristine"
    pristine(ref)
    source = (ref / "test/test_crypto.c").read_text()
    for name in TESTS:
        if f"STU({name})," not in source:
            raise RuntimeError("unknown upstream test " + name)
    cc = os.environ.get("CC", "cc")
    baseflags = ["-std=c11", "-D_DEFAULT_SOURCE", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                 "-ffreestanding", "-fno-builtin", "-fPIC", "-mno-red-zone",
                 "-DBR_LE_UNALIGNED=0", "-DBR_BE_UNALIGNED=0",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    includes = ["-I" + str(ROOT / "userland/libos64/include"), "-I" + str(ROOT / "abi/include")]
    for variant in ("reference", "adapted"):
        print("BearSSL host: building " + variant, flush=True)
        out = work / variant
        out.mkdir()
        upstream = ref if variant == "reference" else BASE / "upstream"
        inc = ["-I" + str(upstream / "inc"), "-I" + str(upstream / "src")]
        flags = baseflags + inc + includes
        if variant == "adapted":
            flags += ["-include", str(BASE / "port/config.h"), "-I" + str(BASE / "port/include")]
        sources = sorted((upstream / "src").rglob("*.c"))

        def compile_core(src):
            obj = out / (src.relative_to(upstream).as_posix().replace("/", "_") + ".o")
            run([cc, *flags, "-c", src, "-o", obj], obj.with_suffix(".log"))
            return obj

        with ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as pool:
            objects = list(pool.map(compile_core, sources))
        # Exercise the real os64 string implementations without interposing
        # their compiler aliases on the sanitizer's own hosted libc calls.
        aliases = ["-D" + name + "=bearssl_host_" + name
                   for name in ("memcpy", "memmove", "memset", "memcmp")]
        string_obj = out / "os64_str.o"
        run([cc, *baseflags, *includes, *aliases, "-c", ROOT / "userland/libos64/str.c",
             "-o", string_obj], out / "os64_str.log")
        runtime = out / "runtime.o"
        run([cc, *baseflags, "-c", BASE / "port/runtime.c", "-o", runtime], out / "runtime.log")
        objects += [runtime, string_obj]
        archive = out / "core.a"
        run(["ar", "rcs", archive, *objects], out / "archive.log")
        # Hosted test drivers retain libc; their crypto calls enter the archive.
        driverflags = baseflags + inc + includes
        for name in ("crypto", "x509"):
            exe = out / ("test_" + name)
            run([cc, *driverflags, upstream / ("test/test_" + name + ".c"), archive,
                 "-o", exe], out / (name + "_build.log"))
            print(f"BearSSL host: {variant} upstream {name}", flush=True)
            run([exe, *(TESTS if name == "crypto" else [])], out / (name + ".log"), cwd=upstream)
        exe = out / "foundation"
        run([cc, *driverflags, "-DBEARSSL_PORT_TEST=" + str(int(variant == "adapted")),
             BASE / "test/foundation.c", ROOT / "tools/test_bearssl_host.c", archive,
             "-o", exe], out / "foundation_build.log")
        run([exe], out / "foundation.log")
    for name in ("crypto", "foundation"):
        a = (work / "reference" / (name + ".log")).read_bytes()
        b = (work / "adapted" / (name + ".log")).read_bytes()
        if a != b:
            raise RuntimeError("reference/port output differs: " + name)
    print((work / "adapted/foundation.log").read_text(), end="")
    print("BearSSL host: upstream vectors and differential checks PASS", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="retain artifacts in a new directory")
    args = parser.parse_args()
    if args.output:
        args.output = args.output.resolve()
        args.output.mkdir()
        check(args.output)
    else:
        with tempfile.TemporaryDirectory(prefix="bearssl-host-") as work:
            check(Path(work))
