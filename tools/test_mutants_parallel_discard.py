#!/usr/bin/env python3
"""The guard on what tools/mutants_parallel.py deletes: discard() removes a
worker's copy only when it is a real directory directly inside the run's
directory and carries this run's mark, and refuses everything else.

    python3 tools/test_mutants_parallel_discard.py

discard() and MARK are taken out of the runner's source by its syntax tree,
so the runner itself never runs here. Every case is built in a directory this
test makes with mkdtemp and removes at the end.
"""
import ast
import pathlib
import shutil
import sys
import tempfile

SOURCE = pathlib.Path(__file__).resolve().parent / 'mutants_parallel.py'


def load_discard(out, run):
    tree = ast.parse(SOURCE.read_text())
    keep = [n for n in tree.body
            if (isinstance(n, ast.FunctionDef) and n.name == 'discard')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) == 'MARK' for t in n.targets))]
    if len(keep) != 2:
        sys.exit('test_mutants_parallel_discard: cannot find discard() and MARK in mutants_parallel.py')
    ns = {'out': out, 'RUN': run, 'shutil': shutil, 'print': lambda *a, **k: None}
    exec(compile(ast.Module(body=keep, type_ignores=[]), str(SOURCE), 'exec'), ns)
    return ns['discard'], ns['MARK']


def main():
    base = pathlib.Path(tempfile.mkdtemp(prefix='discard-test.'))
    failed = 0

    def check(ok, what):
        nonlocal failed
        print(('ok   ' if ok else 'FAIL ') + what)
        failed += not ok

    try:
        out = base / 'run'
        out.mkdir()
        run = 'this run\n'
        discard, mark = load_discard(out, run)

        def copy(name, words=run, parent=out):
            root = parent / name
            (root / 'userland').mkdir(parents=True)
            (root / 'userland' / 'f').write_text('x')
            if words is not None:
                (root / mark).write_text(words)
            return root

        def discard(root, _discard=discard):
            # A refusal is the guard's to make: one rmtree raises is not one.
            try:
                _discard(root)
                return True
            except OSError:
                return False

        root = copy('w-0')
        discard(root)
        check(not root.exists(), 'a copy this run made, inside the run, is deleted')

        root = copy('w-1', words=None)
        discard(root)
        check(root.exists(), 'a directory with no mark is kept')

        root = copy('w-2', words='another run\n')
        discard(root)
        check(root.exists(), "a directory with another run's mark is kept")

        deeper = out / 'logs'
        deeper.mkdir()
        root = copy('w-3', parent=deeper)
        discard(root)
        check(root.exists(), 'a marked copy not directly inside the run is kept')

        elsewhere = copy('w-4', parent=base)
        link = out / 'w-4'
        link.symlink_to(elsewhere, target_is_directory=True)
        check(discard(link) and link.is_symlink() and (elsewhere / 'userland' / 'f').exists(),
              'a link to a marked copy is kept, and so is what it names')

        root = copy('w-5', words=None)
        real = base / 'mark-elsewhere'
        real.write_text(run)
        (root / mark).symlink_to(real)
        discard(root)
        check(root.exists(), 'a copy whose mark is a link is kept')

        discard(out)
        check(out.exists(), 'the run directory itself is kept')

        loose = out / 'w-6'
        loose.write_text(run)
        discard(loose)
        check(loose.exists(), 'a file is kept')
    finally:
        shutil.rmtree(base)
    print(f'mutants_parallel discard: {8 - failed} of 8 cases held')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
