# cut

Print selected bytes, UTF-8 characters, or fields from each input line.

```sh
cut -d ' ' -f 2 file.txt
cut -d ':' -f 1,3-5 file.txt
cut -f 2- tab-separated.txt
echo 'red green blue' | cut -d ' ' -f2
cut -c 1-20 file.txt
cut -b 1-80 file.txt
```

Choose one selection option: `-b`/`--bytes`, `-c`/`--characters`, or
`-f`/`--fields`. A LIST contains positions (`2`), inclusive ranges (`2-5`),
prefixes (`-5`), or suffixes (`2-`), separated by commas or single blanks.
Positions start at 1. Selections print in input order; overlapping ranges
do not duplicate data. Out-of-range positions produce an empty selection.
Short options accept attached values, including `-f2`, `-b1-5`, and `-d:`.

The default field delimiter is a tab. `-d`/`--delimiter` takes a single byte;
an empty argument selects NUL. Repeated delimiters create empty fields,
so `cut -d ' ' -f2` treats two adjacent spaces as two separators. Lines
without the delimiter pass through unless `-s`/`--only-delimited` is set.
Carriage returns and embedded NUL bytes are data; CRLF is not rewritten.

`-c` counts UTF-8 code points independently of locale, rather than display
cells or grapheme clusters. Invalid UTF-8 advances one byte and preserves
the original byte. `-b` counts raw bytes. With `-b -n`, a selected character's
final byte includes that complete UTF-8 character, and a selection ending
inside a character omits it.

`--complement` (also `-C`) selects positions outside LIST.
`--output-delimiter=STRING` (also `-O STRING`) joins selected fields, or
selected byte/character ranges. Its default in field mode is the input
delimiter; byte/character mode concatenates selections. An empty STRING
outputs a NUL separator. Distinct adjacent ranges retain a separator;
overlapping ranges merge. These extensions follow the
[coreutils cut interface](https://www.gnu.org/s/coreutils/manual/html_node/cut-invocation.html).

With no FILE, or with `-`, read standard input. Multiple inputs concatenate
without headers; `--` ends option parsing. A final nonempty line without a
newline receives one. `--help` prints the option reference.

The build and disk population discover `apps/cut` automatically. The app
uses buffered I/O and holds the current line in growable storage, so memory
depends on the longest input line rather than a fixed truncation limit.
For a newline field delimiter, the input file is one field record and its
final newline terminates the last field; that mode buffers the entire input.
Allocation and I/O failures report to stderr and return status 1. Invalid
arguments return 2; successful processing and help return 0. File errors
allow remaining files to run; an output failure stops processing.

`tools/test_cut_host.sh` compiles the actual app with the real argument,
formatting, and string routines under ASan/UBSan. It compares byte/field
behavior with host coreutils and checks UTF-8, binary input, long lines,
short I/O, invalid arguments, and injected allocation/I/O failures. Target
runtime verification requires a separate os64 guest run.
