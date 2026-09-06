# os64get and os64serve

`os64get` installs files into a running os64 system. Its main workflow is
`os64get -a HOST`: obtain the build server's catalogue, route files to their
installation directories, download and verify changes, preserve replaced
originals, then install the prepared files. It also supports individual valet
files and HTTP/HTTPS downloads. URL downloads share staging, verification,
publication, and cleanup, but do not back up replaced files.

[`tools/os64serve.py`](tools/os64serve.py) is the build PC's server, sometimes
called the **valet**. It supplies files and source labels; the client's
configuration decides where they belong. It does not serve HTTP.

Start with [refreshing a system](#refreshing-a-system), or jump to
[client commands](#client-commands), [routing](#configuration-and-routing),
[installation steps](#how-installation-works), [backups](#what-the-archive-contains),
[temporary files and cancellation](#temporary-files-space-and-ctrlc),
[server setup](#running-os64serve), [HTTP/HTTPS](#http-and-https-downloads),
or [troubleshooting](#exit-status-and-troubleshooting).

## Refreshing a system

Finish the build on the host, then start the server from the repository root:

```sh
make
python3 tools/os64serve.py userland/bin userland/bin/tests=tests kernel/bin=kernelbin etc
```

This offers applications, shared libraries, test programs, the kernel and
its build outputs, and system configuration. The `tests` and `kernelbin`
labels matter: they let the shipped client configuration send fixtures to
`/tests` while more specific rules send the kernel and libraries elsewhere.
Serving `userland/bin` alone does not include its `tests` subdirectory.

On os64, replace `HOST` with the build PC's reachable hostname or IPv4 address:

```sh
os64get -a HOST
```

With QEMU's user-mode networking, the host address is normally `10.0.2.2`:

```sh
os64get -a 10.0.2.2
```

The client reports changed/unchanged files, installation results, and the
backup run directory when one was created. Stop the host server with Ctrl+C
when finished. A newly installed kernel takes effect after reboot. Replacing
an executable does not restart its running process; on ext2 that process can
continue using its old open image.

The example deliberately does not serve the repository root. The P5's
`limine.conf` is maintained by hand and is excluded from automatic refreshes
([the boot-menu ruling](DEBTS.md#explicitly-not-debts-recorded-so-they-arent-re-litigated)).
A routing rule is not an exclusion rule: if a server offers `limine.conf`,
`-a` will include it.

When upgrading from the client that archived incoming downloads, first fetch
the new client with the old client's archive disabled:

```sh
os64get -n HOST os64get
```

Then use the new client for the batch. This bootstrap command follows the
active routing configuration; give `/bin/os64get` as DEST if necessary.

## Client commands

```text
os64get [-q] [-n] [-f] HOST NAME [DEST]
os64get -a [-q] [-n] [-f] [-c] HOST
os64get [-q] [-n] http[s]://HOST[:PORT]/PATH [DEST]
os64get --help
```

| Option | Effect |
| --- | --- |
| `-a`, `--all` | Install the files in the valet's catalogue. Takes HOST only, without NAME or DEST. |
| `-f`, `--force` | Fetch even when the destination matches the advertised length and CRC. Identical downloaded contents still need no replacement or backup. HTTP already fetches each time. |
| `-n`, `--no-archive` | Disable backups of replaced originals for valet installs. URL downloads already omit backups. |
| `-q`, `--quiet` | Suppress progress. Errors still print; batch mode still prints its installation summary and backup location. |
| `-c`, `--changes-only` | Hide unchanged entries in the batch display; does not change which files are considered. |

Examples on os64:

```sh
os64get HOST os64_kernel
os64get HOST fputest
os64get HOST fputest /tmp
os64get HOST os64get /tmp/os64get-test
os64get -a -c HOST
```

NAME is a single filename, without directories. An explicit DEST that names
an existing directory receives the file under NAME; otherwise DEST is the
full output pathname. The parent directory must exist. Without DEST, the
valet name is routed through configuration. A single-file fetch consults
LIST for its source label when lot rules are configured; if that lookup
fails, it falls back to routing by name.

## Configuration and routing

The client finds `os64get.conf` through the system's
[configuration search path](docs/conf_path.md), controlled by `conf` in
`/etc/os64.conf`. The default is `/home:/etc`: `/home/os64get.conf` wins over
`/etc/os64get.conf`. The first file found supplies the configuration; the
files are not merged. Copy the shipped file before customizing if you want
to retain its other rules.

[`etc/os64get.conf`](etc/os64get.conf) is the complete shipped map. The
syntax is `key = value`, with whitespace around fields and `#` comments.
Routing values name directories; the filename is appended unchanged.

| Precedence | Example | Meaning |
| --- | --- | --- |
| 1: exact name | `os64_kernel = /fat/boot` | Match that filename. |
| 2: suffix | `*.so = /lib` | Match that ending; this is not a general shell glob. |
| 3: source lot | `@tests = /tests` | Match the server's label for the source directory. |
| 4: default | `* = /bin` | Match files not claimed above. |

Within a class the last matching rule wins. Thus `os64_kernel` from
`kernel/bin=kernelbin` goes to `/fat/boot`, a `.so` from the same directory
goes to `/lib`, and a fixture goes to `/tests` through `@kernelbin`.
Explicit DEST overrides routing. With no matching rule, or no configuration
file, the destination is the current directory.

The separate setting below enables replacement backups for valet installs:

```text
archive = /home/archive
```

Omit the setting to disable backups, or use `-n` for one invocation. The
archive directory and its dated subdirectories are created as needed.
Installation directories are not created automatically. Archive and managed
scratch trees cannot overlap, and cannot be used as installation destinations.

The configuration is read once at startup. A batch that replaces
`/etc/os64get.conf` uses the previously loaded rules throughout that batch;
the next invocation reads the new file. An overriding `/home` copy continues
to take precedence.

## How installation works

For `-a`, preparation covers the batch before installation begins:

1. **Catalogue and plan.** Read LIST and resolve destinations. Compare files
   with the advertised length and CRC before reserving scratch, so unchanged
   files need no writes on a read-only or full mount. Unless forced, matching
   entries need no staging reservation. Reject conflicting targets, using
   filesystem-resolved names and the changed entries' reserved staging names
   to identify aliases before receiving payloads.
2. **Download and verify.** Receive changed files into scratch and verify the
   received length and CRC before accepting them. Sync and close the files.
3. **Prepare originals.** Compare downloaded contents with existing targets.
   If they differ and backups are enabled, copy the old destination to
   scratch on the archive's filesystem, sync it, reread it to check length
   and CRC, and finalize the backup without overwriting another backup.
   Check that the original has not changed during preparation.
4. **Recheck and install.** Recheck prepared destinations, then begin the
   installation phase: rename each prepared download onto its destination.
   The renames require no network access or cross-filesystem data copies.
5. **Clean up.** Remove the invocation's remaining temporary files and empty
   run directories, retaining completed backups.

A download or backup preparation failure prevents installation of the batch.
If an installation rename fails after this boundary, the client reports it
and continues attempting the remaining prepared renames. The result can be
a partially installed batch; the summary reports what actually happened.
Single-file valet fetches use the same steps for their one destination. URL
fetches share preparation, publication, and cleanup with backups disabled.

Length plus CRC is the content comparison, not a cryptographic identity
check. The client has no package dependency solver, uninstall manifest,
automatic rollback, or removal of obsolete installed files. The server's
catalogue describes the files to consider installing, not a complete desired
filesystem image. It is also not a build snapshot: finish rebuilding files
before starting a refresh, and avoid simultaneous installers or edits to
their targets. Destination rechecks do not serialize other writers.

## What the archive contains

A valet run groups **old destination contents** under a UTC date, time, task ID,
and collision-checked counter:

```text
/home/archive/YYYY-MM-DD/HHMMSS-TASKID-COUNTER/bin/os64get
/home/archive/YYYY-MM-DD/HHMMSS-TASKID-COUNTER/lib/libos64.so
/home/archive/YYYY-MM-DD/HHMMSS-TASKID-COUNTER/fat/boot/os64_kernel
```

The path below the run directory is the original absolute destination with
its leading slash removed. Explicit renamed destinations are backed up under
their destination names. The bytes preserved are the locally installed
originals, including local modifications, rather than another server copy.

| Situation | Backup |
| --- | --- |
| Destination did not exist | None. |
| Destination matches the incoming length and CRC | None, including a forced download. |
| Destination differs and archiving is enabled | The old destination, verified before replacement. |
| Destination differs with `-n` or no archive setting | None. |
| HTTP/HTTPS download, including replacement of an existing file | None. |

Deleting a large installed file and downloading it again therefore creates
no archive entry. After successful installation and cleanup, the incoming
copy exists at its destination, without a retained download copy elsewhere.

The client reports “originals kept at” only after a backup is finalized. A
failed first backup may leave an empty archive directory, without that message.
Completed backups remain even if later preparation, cancellation, or a rename
fails. Their presence records originals preserved for attempted replacements;
it does not prove that those replacements completed. Backups preserve file
contents; do not treat them as filesystem snapshots with full metadata.
There is no automatic retention limit or pruning of completed backups.

For manual recovery, the subtree identifies where each saved file belongs.
For example, copy a selected run's `bin/prog` back to `/bin/prog` with the
appropriate filesystem tools. Select the run deliberately: the newest backup
may belong to an aborted attempt. Boot-file recovery may require booting a
working entry or accessing the volume from another system.

## Temporary files, space, and Ctrl+C

Incoming files must be staged on their destination's filesystem so the final
move is a rename. The usual scratch locations are:

| Destination filesystem | Scratch base |
| --- | --- |
| Root, including `/bin`, `/lib`, and `/etc`, when `/tmp` is on root | `/tmp/os64get` |
| Separately mounted `/home` | `/home/.os64get-tmp` |
| Separately mounted `/fat` | `/fat/.os64get-tmp` |

More generally, the client uses `/tmp/os64get` if it resolves to the same
mount as the target, otherwise `<mount>/.os64get-tmp`. If `/tmp` is a
separate mount, root destinations use `/.os64get-tmp`.

Exclusive run and per-file directories contain incoming files under their
basenames. Incomplete backup copies use `backup-N.part` in scratch on the
archive's filesystem. These are managed trees, not adjacent `DEST.part`
files. Runs reserve their own names instead of overwriting another run's
scratch files.

During preparation, Ctrl+C requests cancellation. The client closes its
resources and cleans up across the run, leaving installed files untouched.
After installation renames begin, Ctrl+C is deferred until the rename phase
and cleanup finish. It does not interrupt the batch halfway through its
planned moves. Ordinary handled errors also go through cleanup; failures to
remove a temporary file are reported with its path. Empty scratch base
directories may remain.

A crash, forced kill, or power loss can leave files inside these scratch
trees. There is no automatic boot cleanup, stale-run scavenging, or download
resume. Once no installer is running, abandoned run directories can be
removed manually. The client does not sweep legacy `.part` files elsewhere.

Plan free space for the batch's incoming files on each destination filesystem
while originals are still installed, plus the replaced originals on the
archive filesystem. A large new `/home` file stages on `/home`, without an
extra root-filesystem copy. Existing running executables can retain storage
for their old open images after replacement.

The installation is a sequence of file replacements, not an atomic system
transaction. Ext2 replacement has no missing-name window, but this is not a
power-loss durability guarantee. FAT uses remove-first replacement and can
lose the destination name if replacement fails. Backups provide recovery
contents; they do not remove these filesystem or batch limits.

## Running os64serve

The server uses Python 3's standard library and runs in the foreground:

```text
python3 tools/os64serve.py [PATH|PATH=LOT ...] [--port 6464] [--bind 0.0.0.0]
```

Directories are searched in command-line order, with the first usable file
of a given name winning. Avoid duplicate names across served directories.
Serving is not recursive; name each subdirectory separately. Regular files
are served; final-component symlinks and special files are refused. Without
directory arguments the server serves its current directory and prints a
warning. That directory's files become installation candidates for `-a`.

`--bind` selects the listening IPv4 address; the default listens on all
interfaces. `--port` changes the server port, but **the current os64get valet
client uses port 6464 with no override**. Leave the server at 6464 for it.
The server has no authentication or encryption, and CRC detects corruption
rather than authenticating the source. Its reachable clients can read the
offered files; use the intended build host and network.

The server handles one connection at a time and applies a 30-second socket
timeout. It reads a complete file into host memory to compute length and CRC
over the same bytes it sends. LIST also reads files to calculate checksums;
these payloads are not cached for later GET requests. Large files therefore
need host memory as well as client staging space, and slow sends can time out.

### Windows host with a WSL build tree

If the P5 cannot reach the listener inside WSL, run Windows Python and serve
the WSL files through UNC paths. From a **WSL Bash shell**, substitute the
distro and user names in this example:

```sh
cd /mnt/c/temp
python3.exe '\\wsl$\<distro>\home\<you>\src\os64\tools\os64serve.py' \
  '\\wsl$\<distro>\home\<you>\src\os64\userland\bin' \
  '\\wsl$\<distro>\home\<you>\src\os64\userland\bin\tests=tests' \
  '\\wsl$\<distro>\home\<you>\src\os64\kernel\bin=kernelbin' \
  '\\wsl$\<distro>\home\<you>\src\os64\etc'
```

Use an existing Windows-visible working directory; `/mnt/c/temp` is an
example. `wsl.exe -l` lists distro names. Python's firewall permissions must
allow the P5 to reach TCP 6464. This serves the build tree in place without
copying it to Windows. Bash backslash continuations are not `cmd.exe` syntax;
do not paste this multiline command unchanged into a different shell.

### Wire protocol and limits

Each request uses a new TCP connection. Lines are ASCII with LF endings;
file bodies are raw bytes:

```text
client: LIST\n
server: NAME LENGTH CRC LOT\n
        NAME LENGTH CRC LOT\n
        .\n

client: GET NAME\n
server: OK LENGTH CRC\n
        <exactly LENGTH bytes>
     or NO reason\n
```

LENGTH is decimal bytes; CRC is eight hexadecimal digits of CRC-32/ISO-HDLC
(`zlib.crc32` on the host, `os64_crc32` in os64). LOT is `-` for an unlabeled
directory. The client also accepts older LIST entries without lot metadata.

For batch service, use ASCII filenames without whitespace, at most 63 bytes,
and lot labels without whitespace, at most 31 bytes. A catalogue can contain
at most 256 files. Client pathname buffers allow 255 bytes, including expanded
scratch and backup paths, so the usable destination length can be shorter.
Configuration has 16 slots per exact-name, suffix, and lot rule class, with
directory values of at most 127 bytes. These are implementation limits, not
filesystem limits.

## HTTP and HTTPS downloads

```sh
os64get http://example.com/ /home/page.html
os64get http://example.com/files/readme.txt
```

URLs bypass filename routing. With no DEST, the original URL's last path
segment becomes the filename in the current directory; the query is omitted,
and a path ending in `/` uses `index.html`. An existing DEST directory receives
that filename. Redirects do not change the chosen destination. URL downloads
do not consult `os64get.conf` or make replacement backups, even when the
destination exists and the configuration enables archiving. Managed scratch,
verification, publication, and cancellation still apply. `-n` has no additional
effect; `-a` is unavailable for URLs.

The HTTP reader supports Content-Length, chunked, and connection-close body
framing, follows up to five redirects, and decodes `Content-Encoding: gzip`.
Unsupported coding and malformed or detectably truncated bodies are refused
before installation. Close-delimited bodies have no independent expected
length. Gzip output remains provisional until member CRC/size and trailing
data checks pass. Its decoded size limit is 100 times a known wire length,
with a 1 MiB floor and 16 MiB ceiling; without a known length the limit is
16 MiB. These expansion limits do not apply to identity bodies.

os64 has no native TLS here. HTTPS requires a terminating proxy, such as
[`tools/tlsproxy.py`](tools/tlsproxy.py). For a QEMU guest, start it on the host:

```sh
python3 tools/tlsproxy.py
```

Then in os64:

```sh
export https_proxy=http://10.0.2.2:8888/
os64get https://example.com/
```

For the P5, use the proxy host's reachable LAN address. Lowercase
`https_proxy` applies to HTTPS, `http_proxy` to HTTP, and `no_proxy` supplies
comma-separated bypass hosts/domain suffixes or `*`. Bypassing the proxy
does not make direct HTTPS available. The proxy must accept absolute-form
HTTPS requests and fetch upstream itself; an ordinary CONNECT-only proxy
does not supply this behavior. Only the proxy-to-origin leg uses TLS: the
guest-to-proxy leg and data inside the proxy are plaintext.

## Exit status and troubleshooting

| Status | Meaning |
| --- | --- |
| 0 | Success, including an unchanged destination. |
| 2 | Invalid arguments, or a rejected batch plan such as duplicate destinations. |
| 3 / 4 | Connection failed / request could not be sent. |
| 5 / 6 | Server refusal or unsuccessful HTTP status / invalid response header or framing. |
| 7 / 8 | Short transfer / corrupt data; batch transfer failures are aggregated as 8. |
| 9 | Local write, staging, initialization, or cleanup failure. |
| 10 | Installation rename failed; some files may have installed. |
| 11 | Backup preparation or destination recheck failed. |
| 13 / 14 / 15 | Unusable URL or missing HTTPS route / unsupported coding or gzip expansion limit / failed redirect chain. |
| 130 | Ctrl+C requested cancellation, or was deferred through successful installation. |

Status 12 is an internal unchanged marker, not a public exit status. A
deferred Ctrl+C retains an installation error if one occurred. Read the
diagnostics and batch summary: 130 does not by itself mean nothing installed,
and a cleanup error can follow successful installation.

- **Fixtures went to `/bin`:** check the active config and the server's
  `=tests` / `=kernelbin` arguments. The client warns when lot rules exist
  but the server labels none of its files.
- **A file is missing from the catalogue:** check explicit served directories,
  duplicate names, regular-file status, and filename/catalogue limits.
- **Connection refused or timed out:** check the host address, listener,
  port 6464, bind address, and host firewall; the server logs connections.
- **Preparation failed:** check free space on the destination and archive
  mounts, writable scratch locations, existing destination directories, and
  the reported path. A required backup failure prevents installation.
- **Rename or cleanup failed:** inspect the reported paths and summary before
  retrying. Completed originals remain in the printed backup run directory.

## Implementation and validation references

- [`os64get.c`](userland/apps/os64get/os64get.c): options, configuration,
  GET/LIST, URL handling, batch orchestration, and exit status.
- [`install.c`](userland/apps/os64get/install.c) and
  [`install.h`](userland/apps/os64get/install.h): path planning, filesystem
  scratch selection, backups, destination checks, publication, and cleanup.
- [`http.c`](userland/apps/os64get/http.c): HTTP parsing and body framing.
- [`OS64GET_ARCHIVE_PLAN.md`](OS64GET_ARCHIVE_PLAN.md): installation design,
  failure analysis, and recorded host/guest validation.
- [`tools/test_os64get_host.sh`](tools/test_os64get_host.sh) and
  [`tools/test_http_host.sh`](tools/test_http_host.sh): installer and HTTP
  regression suites.
