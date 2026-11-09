# mkdir

`mkdir [-pv] DIR...` creates directories. Without `-p`, a missing parent or
an existing name is an error.

- `-p`, `--parents`: create missing parents and accept directories that
  already exist. An existing file is still an error.
- `-v`, `--verbose`: print `mkdir: created directory 'PATH'` to standard
  output for each directory created, including new parents. Existing
  directories accepted by `-p` produce no output.
- `-h`, `--help`: show usage and options.
- `--`: end options, allowing names beginning with `-`.

Options can be combined (`-pv`) and can appear before or after operands.
For example:

```sh
mkdir -pv /home/projects/notes
mkdir --parents --verbose drafts/2026 archive/2026
mkdir -- -private
```

The command checks options before creating directories, so help or an
invalid option after an operand does not create that directory. It attempts
later operands after a filesystem error and returns nonzero if an operand
fails. Directories already created are retained when a later step fails.

Parent creation accepts absolute and relative paths, repeated or trailing
slashes, and `.`/`..` components. It visits prefixes in order:
`mkdir -p a/../b` creates `a` if needed before creating `b`; a file at `a`
causes an error. Empty paths and operands of `OS64_PATH_MAX` bytes or more
are rejected without creating their parents. The kernel also enforces its
limit on the resolved absolute path.

Focused host regression tests: `bash tools/test_mkdir_host.sh`.
