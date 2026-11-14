#!/usr/bin/env python3
"""Run every host suite in tools/ and say which passed.

    tools/host_suites.py                     run them all, two at a time
    tools/host_suites.py --list              print the manifest and stop
    tools/host_suites.py --only fetch,tls    suites whose name holds one of these
    tools/host_suites.py --jobs 4 --timeout 1800 --out /tmp/suites

There are over two hundred `tools/test_*.sh` and `tools/test_*.py` files and,
before this script, no one thing that ran them all; a checkpoint merge to
master (docs/releases/) wants one answer, not two hundred. THE MANIFEST IS
EXPLICIT because the files are not uniform: most are suites that take no
arguments, some are variants a flag selects (the mutant harnesses name
those), some need an output directory, some are peers or servers a guest
drives, some are imported by other suites, and the `*_mutants.py` files are
harnesses that take hours and are not run here (CLAUDE.md: mutants run once
per slice, at its end). Every test_* file in tools/ must be accounted for in
exactly one place below, or this script refuses to start and names it: a
suite added without a line here is a suite nobody runs.

Each suite runs with the tree's top as its working directory, its own log in
--out, HOST_BUILD_JOBS capped so two suites do not each take every CPU, and a
deadline. A suite that cannot run on this machine (a Python module missing)
is a SKIP with its reason, printed as loudly as a failure, never a silent
pass. The exit status is nonzero when any suite failed or timed out.
"""
import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / 'tools'

# ── The manifest ────────────────────────────────────────────────────────
#
# A suite is (name, argv, prerequisites). argv is relative to the tree's
# top. A prerequisite is 'py:<module>' (a Python module the suite imports)
# 'bin:<program>' (one it execs) or 'dir:<path>' (a tree under the top it
# reads, such as the fetched WPT checkout); a missing one makes the suite a
# SKIP.
# `OUT` in argv is replaced by a fresh directory under --out (suites that
# write an output directory they are given).

def sh(name, *args, needs=()):
    return (f'{name}' + (' ' + ' '.join(args) if args else ''),
            ['bash', f'tools/{name}.sh', *args], tuple(needs))

def py(name, *args, needs=()):
    return (f'{name}' + (' ' + ' '.join(args) if args else ''),
            ['python3', f'tools/{name}.py', *args], tuple(needs))

# Shell suites that run with no arguments. Every tools/test_*.sh not named
# in SH_WITH_ARGS or SKIPPED below is expected here, and the tripwire
# fills the list from the directory so a new .sh is run by default.
SH_WITH_ARGS = [
    # Variants a flag selects, as the mutant harnesses invoke them: the
    # probe beneath each takes the flag, so a bare run is one group and
    # the flagged runs are the others.
    sh('test_dom_host', '--custom'),
    sh('test_dom_host', '--observer'),
    sh('test_dom_host', '--web-animations'),
    sh('test_libflow_host', '--cliptext'),
    sh('test_libflow_host', '--floats'),
    sh('test_libflow_host', '--motion'),
    sh('test_libflow_host', '--origin'),
    sh('test_libflow_host', '--transforms'),
    # Suites that write into a directory they are given.
    sh('test_canvas_gallery', 'OUT'),
    sh('test_layout_measure', 'OUT'),
    sh('test_layout_graft', 'OUT'),
    sh('test_png_encode_host', 'OUT'),
    # The XML suite's default step reaches for a network; the local half
    # is the regression suite.
    sh('test_xml_host', '--local-only'),
    # The import-maps suite reads WPT's import-maps directory.
    sh('test_importmap_host', needs=('dir:.vendor/wpt/import-maps',)),
    # The WPT runner's own pages, run beside the fetched checkout's resources.
    sh('test_yonder_wpt_host', '--selftest', needs=('dir:.vendor/wpt',)),
]

# Python suites run directly. Not listed: .py files a .sh above execs or
# imports (they run inside it), peers and servers (SKIPPED), harnesses.
PY_SUITES = [
    py('test_bearssl_host'),
    py('test_browser_host'),
    py('test_directory_host'),
    py('test_file_dialog_host'),
    py('test_font_config_host'),
    py('test_font_contracts'),
    py('test_font_family_host'),
    py('test_font_provider_host'),
    py('test_font_psf2_host'),
    py('test_freetype_host'),
    py('test_freetype_target_runtime'),
    py('test_garb_differential', needs=('py:tinycss2',)),
    py('test_garb_select', needs=('py:tinycss2', 'py:cssselect2', 'py:html5lib')),
    py('test_gclock_host'),
    py('test_gif_host'),
    py('test_gif_sequence_host'),
    py('test_gopher_request_host'),
    py('test_gterm_fonts_host'),
    py('test_jpeg_host'),
    py('test_js_image'),
    py('test_js_prepare'),
    py('test_js_teardown_audit'),
    py('test_math_audit'),
    py('test_mutants_parallel_caps'),
    py('test_mutants_parallel_discard'),
    py('test_memory_try_host'),
    py('test_pty_budget_host'),
    py('test_pulse_ui_host'),
    py('test_roam_launch_host'),
    py('test_sound_audit'),
    py('test_sbc_audit'),
    py('test_roam_ui_host'),
    py('test_scribe_host'),
    py('test_spawn_argv_host'),
    py('test_svg_core', '--output', 'OUT'),
    py('test_svg_geometry', '--output', 'OUT'),
    py('test_syscall_lifetime_host'),
    py('test_task_hold_host'),
    py('test_task_wait_host'),
    py('test_tcp_write_syscall_host'),
    py('test_text_host'),
    py('test_tls13_engine_host', needs=('bin:openssl',)),
    py('test_tls13_vectors_host'),
    py('test_tls_engine_host'),
    py('test_tls_inputs_host'),
    py('test_tls_policy_host', needs=('py:cryptography', 'bin:openssl')),
    py('test_tls_profile_host'),
    py('test_tls_store_host'),
    py('test_tls_transport_host'),
    py('test_ui_action_host'),
    py('test_ui_action_host', '--menu'),
    py('test_ui_dataview_host'),
    py('test_ui_menu_mutations'),
    py('test_ui_text_host'),
    py('test_ui_text_host', '--real'),
    py('test_webp_host', '--output', 'OUT'),
    py('test_window_pin_host'),
    py('test_window_size_host'),
    py('test_window_placement_host'),
    py('test_tracker'),
]

# Files in tools/test_* that this script does not run, each with why.
SKIPPED = {
    # Run by another suite (exec'd or imported), so they are covered above.
    'test_cut_host.py': 'run by test_cut_host.sh',
    'test_sort_host.py': 'run by test_sort_host.sh',
    'test_random_host.py': 'run by test_random_host.sh (emits its vectors)',
    'test_tcp_host.py': 'run by test_tcp_host.sh',
    'test_sshd_host.py': 'run by test_sshd_host.sh',
    'test_sshd_session_host.py': 'imported by test_sshd_host.py',
    'test_sysstat_json.py': 'run by test_sysstat_host.sh',
    'test_js_port_target.py': 'run by test_js_port_target.sh',
    'test_html_host.py': 'run by test_html_host.sh',
    'test_html_fuzz.py': 'run by test_html_host.sh',
    'test_html_corpus.py': 'run by test_html_host.sh',
    'test_garb_suite.py': 'run by test_garb_host.sh',
    'test_xml_differential.py': 'run by test_xml_host.sh',
    'test_layout_export.py': 'run by the test_layout_*.sh suites',
    'test_yonder_corpus_diff.py': 'run by test_yonder_replay_host.sh',
    'test_yonder_records.py': 'run by test_yonder_replay_host.sh',
    'test_yonder_tally.py': 'run by test_yonder_replay_host.sh',
    # Peers and servers that a guest or another process drives.
    'test_tcp_close.py': 'a peer for /tests/netclose in the guest',
    'test_tcp_write_peer.py': 'a peer for /tests/tcpwriteprobe in the guest',
    'test_tls_transport_peer.py': 'a peer for tlstransportprobe in the guest',
    'test_os64get_https_peer.py': 'a peer for os64get in the guest',
    'test_libpage_refresh_server.py': 'a manual QEMU/wend fixture server',
    # Suites that drive a guest, not the host.
    'test_early_exceptions.py': 'boots QEMU instances',
    'test_telnetd_limits.py': 'needs an idle guest',
    'test_telnetd_storage.py': 'needs an idle guest',
    # Suites that need an input this script cannot supply.
    'test_layout_stream.sh': 'needs an ARTICLE file and an output directory',
    'test_yonder_replay_host.sh': 'needs a kept-pages corpus (KEPT-DIR OUT-DIR)',
    'test_yonder_wpt_host.sh': 'a WPT directory needs the tree tools/wpt_fetch.sh fetches; its --selftest runs above',
}

HARNESS = re.compile(r'^test_.*_mutants\.py$')
# Not suites: fixtures, headers, drivers the suites compile.
NOT_SUITES = re.compile(r'\.(c|h|inc|json|husk)$')


def manifest():
    """Every suite to run, in a stable order: the bare shell suites found
    in tools/, then the argument-taking ones, then the Python ones."""
    sh_args_names = {argv[1].split('/')[-1] for _, argv, _ in SH_WITH_ARGS}
    bare = []
    for p in sorted(TOOLS.glob('test_*.sh')):
        if p.name in SKIPPED or p.name in sh_args_names:
            continue
        bare.append(sh(p.stem))
    return bare + SH_WITH_ARGS + PY_SUITES


def tripwire():
    """Name every tools/test_* file that is neither run nor skipped."""
    accounted = set(SKIPPED)
    for _, argv, _ in SH_WITH_ARGS + PY_SUITES:
        accounted.add(argv[1].split('/')[-1])
    missing = []
    for p in sorted(TOOLS.glob('test_*')):
        if p.is_dir() or NOT_SUITES.search(p.name) or HARNESS.match(p.name):
            continue
        if p.suffix == '.sh' and p.name not in SKIPPED:
            continue        # a bare .sh runs by default
        if p.name not in accounted:
            missing.append(p.name)
    stale = [n for n in SKIPPED if not (TOOLS / n).exists()]
    for _, argv, _ in SH_WITH_ARGS + PY_SUITES:
        if not (ROOT / argv[1]).exists():
            stale.append(argv[1])
    return missing, stale


def missing_prerequisite(needs):
    for need in needs:
        kind, _, what = need.partition(':')
        if kind == 'py' and importlib.util.find_spec(what) is None:
            return f'python module {what} is not installed'
        if kind == 'bin' and shutil.which(what) is None:
            return f'{what} is not on PATH'
        if kind == 'dir' and not (ROOT / what).is_dir():
            return f'{what} is not in the tree'
    return None


def run_one(suite, out_dir, timeout, build_jobs):
    name, argv, needs = suite
    log = out_dir / (re.sub(r'[^A-Za-z0-9_.-]+', '_', name) + '.log')
    why = missing_prerequisite(needs)
    if why:
        log.write_text(f'SKIP: {why}\n')
        return name, 'SKIP', 0.0, why
    argv = [a if a != 'OUT' else str(out_dir / (name.split()[0] + '.out')) for a in argv]
    env = dict(os.environ, HOST_BUILD_JOBS=str(build_jobs), MAKEFLAGS=f'-j{build_jobs}')
    start = time.monotonic()
    with open(log, 'wb') as f:
        f.write((' '.join(argv) + '\n\n').encode())
        f.flush()
        try:
            proc = subprocess.run(argv, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT,
                                  env=env, timeout=timeout, start_new_session=True)
            verdict = 'PASS' if proc.returncode == 0 else 'FAIL'
            detail = '' if proc.returncode == 0 else f'exit {proc.returncode}'
        except subprocess.TimeoutExpired:
            verdict, detail = 'TIMEOUT', f'past {timeout}s'
    return name, verdict, time.monotonic() - start, detail


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--list', action='store_true', help='print the manifest and the skips, run nothing')
    ap.add_argument('--only', help='comma-separated substrings; run the suites whose name holds one')
    ap.add_argument('--jobs', type=int, default=2, help='suites at once (default 2: one heavy job per '
                    'machine is the house rule, and a suite is its own parallel build)')
    ap.add_argument('--build-jobs', type=int, default=4, help='HOST_BUILD_JOBS and make -j inside each suite')
    ap.add_argument('--timeout', type=int, default=1800, help='seconds per suite (default 1800)')
    ap.add_argument('--out', help='where the logs go (default: a new directory under $TMPDIR)')
    args = ap.parse_args()

    missing, stale = tripwire()
    for n in missing:
        print(f'{n}: in tools/ but neither run nor skipped by tools/host_suites.py — add it', file=sys.stderr)
    for n in stale:
        print(f'{n}: named by tools/host_suites.py but not in the tree — remove it', file=sys.stderr)
    if missing or stale:
        return 2

    suites = manifest()
    if args.only:
        keys = [k.strip() for k in args.only.split(',') if k.strip()]
        suites = [s for s in suites if any(k in s[0] for k in keys)]
    if args.list:
        for name, argv, needs in suites:
            print(f'{name:<40} {" ".join(argv)}' + (f'   needs {", ".join(needs)}' if needs else ''))
        print(f'\n{len(suites)} suites; skipped:')
        for n, why in sorted(SKIPPED.items()):
            print(f'  {n:<36} {why}')
        return 0

    out_dir = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix='os64-host-suites-'))
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f'{len(suites)} suites, {args.jobs} at a time, {args.timeout}s each, logs in {out_dir}', flush=True)
    results = []
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_one, s, out_dir, args.timeout, args.build_jobs) for s in suites]
        for fut in futures:
            name, verdict, secs, detail = fut.result()
            results.append((name, verdict, secs, detail))
            print(f'{verdict:<7} {secs:7.1f}s  {name}' + (f'  ({detail})' if detail else ''), flush=True)

    counts = {k: sum(1 for r in results if r[1] == k) for k in ('PASS', 'FAIL', 'TIMEOUT', 'SKIP')}
    print(f'\n{counts["PASS"]} passed, {counts["FAIL"]} failed, {counts["TIMEOUT"]} timed out, '
          f'{counts["SKIP"]} skipped, in {(time.monotonic() - started) / 60:.1f} min; logs in {out_dir}')
    for name, verdict, _, detail in results:
        if verdict != 'PASS':
            print(f'  {verdict:<7} {name}  {detail}')
    summary = out_dir / 'SUMMARY.txt'
    summary.write_text(''.join(f'{v}\t{s:.1f}\t{n}\t{d}\n' for n, v, s, d in results))
    return 1 if counts['FAIL'] or counts['TIMEOUT'] else 0


if __name__ == '__main__':
    sys.exit(main())
