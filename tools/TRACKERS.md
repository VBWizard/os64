# Feature trackers

`tracker.py` validates and renders the shared six-column CSV format used by
HTTP/2 and iframes. Each arc keeps its own data; another arc needs a CSV,
not another renderer or launcher.

```sh
tools/tracker.sh iframe
tools/tracker.sh http2 /path/to/worktree
python3 -I tools/tracker.py --csv docs/design/pending/IFRAME_TRACKER.csv --quiet
python3 -I tools/tracker.py --csv path/to/FEATURE_TRACKER.csv --html /tmp/feature.html
```

The launcher reads `docs/design/pending/<ARC>_TRACKER.csv` from the selected
tree (its own tree by default), writes `~/<arc>_tracker/index.html`, and
opens Firefox. `TRACKER_NO_OPEN=1` skips the browser;
`TRACKER_OUTPUT_DIR=/path/to/directory` overrides the output directory.
It uses the renderer alongside the launcher, so that renderer can also read
CSVs from older trees. Re-run the command to refresh the generated page.
`tools/http2_tracker.py` preserves HTTP/2's default path, title, ID prefix,
and command-line options, including use from an existing home launcher.

The header is `id,task,depends_on,status,done_in,notes`. Dependencies are
space-separated IDs. Quote fields containing commas. Blank lines between
slices help keep edits apart. IDs must be unique; references must exist.
The checker also rejects malformed field counts, missing task text, invalid
statuses, completed rows without a PR reference, and rulings without notes.

| Row | ID | Statuses |
|---|---|---|
| Slice | `PREFIX.n`, e.g. `F.1` or `H2.1` | `todo`, `design`, `design-review`, `code`, `review`, `p5`, `done`, `dropped` |
| Optional sub-task | `PREFIX.n.m` | `todo`, `doing`, `done`, `dropped` |
| Optional capture | `Cn` | Same as sub-task |
| Optional question | `Qn` | `open`, `ruled` |

The prefix starts with a letter and may contain letters, digits, underscores
or hyphens. `--prefix` restricts it when an arc needs that check. `--title`
overrides the title derived from the CSV filename; `--contract` adds an
upkeep-document reference to the page. Sub-tasks are grouped with their
parent slice regardless of CSV order. Questions and captures have separate
sections. Slice progress excludes dropped rows. A slice without sub-tasks
shows its lifecycle status without an empty task counter; optional sub-task
progress is reported separately from slice completion.

The CSV is the editable source; HTML is generated. Status and acceptance
rules belong to the arc: [HTTP2.md § 0](../docs/design/pending/HTTP2.md#0-the-tracker--how-http2_trackercsv-is-kept)
and [the iframe tracker contract](../docs/design/pending/IFRAME_TRACKER.md).
