# Native commands

The system image installs standalone user commands in `/bin`, supervised
service programs in `/core`, and command help records in `/share/help`. In
Shoot, `help` lists only Shoot's built-in commands; use a standalone command's
`-h` or `--help` option for its concise usage record. The
tables below group the command surface by purpose; they do not imply POSIX
compatibility. Most standalone commands accept `-h` or `--help` as shown by
their help record.

The system image installs long-form application manuals in `/share/info/`.
Run `info shoot` or `info reed` to read them in the fullscreen pager. These
manuals describe application behavior and configuration; they are separate
from the short, noninteractive records in `/share/help/`.

Quick-help records use `Usage` for invocation forms and `Options` only for
flags accepted at launch. Interactive controls appear under `Keybindings`,
application-internal commands under `Commands`, and relevant constraints or
limits under `Notes`. Empty sections are omitted. Existing records with only
`usage` and `option` fields remain valid; repeated `keybinding`, `command`, and
`note` fields supply the corresponding optional sections. Text wraps to the
terminal width, while complete long words are preserved.

Native commands that use the shared option parser accept grouped boolean short
options (for example, `list -lt`), attached or separated values for
argument-taking short options (`ping -c20` and `ping -c 20`), and long values
as either `--name=value` or `--name value`. A value-taking short option
consumes the rest of its short-option group as its value and ends that group;
for example, `ping -ci` means `-c i`, not `-c -i`. Its following argument is
consumed as a value even when it begins with `-`, so negative numeric values
reach the option's own validator. `--` ends option parsing and makes later
hyphen-leading arguments operands. Options may appear between operands until
that marker. Unknown options and missing values fail before command work
begins. The shared parser is used by `list`, `del`, `ping`, `lspci`, `lsusb`,
and `lsdsk`. The `--` marker is also accepted by path-operand commands
`copy`, `move`, `plant`, `mkdir`, and `type` so a filename beginning with `-`
can be supplied unambiguously. This does not add flags to those commands.

## Shoot built-ins

These commands run inside Shoot and are not standalone `/bin` programs:
`jump` changes the working directory, `help` lists built-in commands,
`history` displays or manages the current session's history, `reload` reloads
Shoot configuration, and `exit` or `logout` leaves the current login session.
See the [history contract](../shell/history.md) for history options and
persistence.

## Files and editing

| Command | Purpose |
| --- | --- |
| `list [ -l | --long ] [ -t ] [ --tree DEPTH ] [path...]` | List each operand in order; long metadata and bounded tree views are available. |
| `copy SOURCE... DESTINATION` | Copy one or more regular files. With multiple sources, the destination must be an existing directory and each file keeps its basename. Existing destinations are never overwritten. Directory sources are not copied. |
| `move SOURCE... DESTINATION` | Move or rename one or more files or directories. With multiple sources, the destination must be an existing directory and each source keeps its basename. Existing filesystem move safeguards apply; batches may partially succeed. |
| `plant NAME...` | Create one or more empty files. Existing files are never replaced; failures are reported per operand and the command returns failure if any target fails. |
| `del [-r|--recursive] TARGET...` | Remove each file or directory target in order. Directories are nonrecursive by default; `-r` explicitly removes trees. Failures are reported per target and later targets are still attempted; duplicates or targets already removed by an earlier recursive operand fail explicitly. |
| `type FILE` | Display a file. |
| `view FILE` | Open a regular text file in the read-only fullscreen viewer; see the [pager controls and limits](text-pager.md). |
| `info PROGRAM` | Read the installed fullscreen manual for one program from `/share/info/`; manuals are separate from quick help. |
| `locate NAME` | Show how a command name resolves. |
| `reed [FILE]` | Start the [Reed editor](../editor/README.md). |

`del` consumes its argv directly without a separate 64-target array.
`del -r` plans at most 4,096 entries and 32 directory levels before deleting
planned contents. Mount points, the current directory, and permission checks
remain protected by the kernel. Empty child directories may be removed during
preflight; failures are reported and return nonzero, but recursive removal is
not an atomic filesystem transaction.

`list` is Mangrove's directory-listing program and follows the shared native
option conventions above. `-t` selects tree output to the maximum depth of 16;
the former `-t DEPTH` and `-tDEPTH` forms remain accepted when the attached or
following token is a decimal depth from 1 to 16. `--tree DEPTH` selects a
specific depth explicitly. With multiple operands, each target uses the selected display
mode in operand order; all options are validated before listing begins. A
failed target makes the overall command fail while later targets are still
attempted. Pathnames use Mangrove's native byte-length and
anchored-working-directory rules; see the
[pathname and process contracts](../architecture/processes-and-syscalls.md).
Simple names are shown without quotes. Names containing whitespace, quotes,
backslashes, control characters, invalid UTF-8, or recognized Unicode
whitespace/invisible format characters are enclosed in single quotes. Inside
that display form, apostrophes, double quotes, and backslashes are escaped
with a backslash; tab, newline, carriage return, backspace, form feed, and
vertical tab use `\\t`, `\\n`, `\\r`, `\\b`, `\\f`, and `\\v`; other
controls and invisible Unicode scalars use `\\u{HEX}`; malformed UTF-8 bytes
use `\\xHH`. These are unambiguous display escapes, not changes to the stored
filename. Valid Unicode scalars currently count as one terminal cell, matching
Mangrove's documented scalar-based terminal layout. Escaped quote characters
and directory suffixes are included in the column-width calculation.

## Devices and volumes

| Command | Purpose |
| --- | --- |
| `lspci [-v]`, `lsusb [-v]` | Inspect PCI and USB device snapshots. |
| `lsdsk [-v]` | Show disks, partitions, filesystems, labels, mountpoints, and optionally whole-disk details. |
| `mount DEVICE` | Request a volume mount using volumed's `/vol` naming policy. |
| `unmount DEVICE` | Request a flush and unmount; a busy volume remains mounted. |
| `eject DEVICE` | Cleanly unmount and suppress automatic remount of a removable device. This is not physical USB removal. |
| `diskutil` | Enter the protected interactive disk inspection and management utility. Destructive operations require storage authorization. |

Device names and identities are runtime values, not stable disk identifiers.
Volume requests and removal behavior are specified in
[storage lifecycle](../storage/lifecycle.md).

`diskutil` has its own interactive `help` command. Select a whole disk before
running `info` or a mutating command. Its operations include `format
<partN|disk> <fat32|mgfs|exfat>`, `label <partN|disk> <label>`,
`initialize gpt`, `create <size|rest>`, and `delete <partN>`. Formatting,
partition-table initialization, partition creation, and deletion require
storage authorization and exact typed confirmation; system-managed, mounted,
read-only, or changed targets are rejected. Whole-disk format is refused when
the disk still has partitions. These safeguards do not make destructive
commands suitable for non-disposable media without careful target selection.

## System and session information

| Command | Purpose |
| --- | --- |
| `about`, `version` | Show OS and system details, or print the version. |
| `identity` | Show the current process identity. |
| `where` | Print the current working directory. |
| `crew` | List processes in the current Shoot session. |
| `mem` | Show physical-memory use. |
| `uptime`, `time COMMAND [ARGUMENTS...]`, `date`, `wait SECONDS` | Show elapsed uptime, time a child command, show UTC time, or wait. |
| `logv [boot|SOURCE]` | Display logs, optionally limited to the current boot or one source. |
| `tmon` | Display a live task and system monitor. |
| `power` | Show battery and AC status when available. |
| `sprout status [SERVICE]` | Inspect service state. `start`, `stop`, `restart`, and supported `reload` operations require service-control authorization. |
| `user` | List, show, create, remove, change role, or set account passwords, subject to identity privileges and PASS. |
| `clear`, `say TEXT...` | Clear the visible terminal or print text. |
| `shutdown`, `reboot` | Request an orderly system power-off or restart. They display `Shutting down...` or `Restarting...` before the request. Active applications and leaf services receive a bounded cleanup window; mounted filesystems are synchronized before the ACPI transition. A cleanup timeout or sync error cancels the request and reports failure. |

## Network

| Command | Purpose |
| --- | --- |
| `netinfo [status [INTERFACE]|interfaces|routes|neighbors|connections|dns]` | Inspect network state. |
| `netcfg COMMAND` | Configure automatic/manual addressing, enable/disable an interface, renew DHCP, or reload configuration. Mutations use networkd's authorization policy. |
| `ping [-c|--count COUNT] [-i|--interval SECONDS] HOST` | Send ICMP echo requests; the default interval is one second and intervals accept fractions down to 0.002 seconds. |
| `resolve HOSTNAME` | Resolve an IPv4 address. |
| `fetch [OPTIONS] URL...` | Stream one or more HTTP or authenticated HTTPS downloads to the current directory. |
| `fetch [OPTIONS] --input-file=FILE` | Download URLs listed one per line in a file. |

`ping` accepts an IPv4 literal or resolves a hostname to one IPv4 address per
invocation. The banner and every echo use that same address, including for
hostnames beginning with digits. A resolution failure sends no echo requests.
`-c` / `--count` accepts 1 through 1000 requests. `-i` / `--interval` accepts
decimal seconds, defaults to one second, and rejects values below 0.002
seconds. Fractional intervals round up to the monotonic clock's millisecond
resolution and are scheduled from the first request's start time, avoiding
cumulative processing-time drift. The current ICMP API supports one outstanding
request, so a subsecond interval also bounds an individual echo wait to that
interval; a lost reply therefore does not stretch the next request's cadence.
Ctrl+C interrupts an echo or interval wait, prints transmitted/received counts,
and returns control to Shoot.

`fetch` defaults to `http://` when a URL has no scheme. Explicit `http://` and
`https://` URLs are accepted; other schemes remain unsupported. HTTPS uses
mgTLS TLS 1.3 and requires a valid chain in the system CA store, trusted UTC
for certificate validity, and a matching DNS or IP subject alternative name.
Unavailable or untrusted realtime, a missing or invalid CA store, or any
certificate error fails the download. HTTPS never falls back to HTTP. HTTP
redirects may upgrade to HTTPS; redirects from HTTPS to HTTP are rejected.
Certificate signature-verification failures are reported separately from
other certificate validation failures; both remain fatal.
HTTP defaults to port 80 and HTTPS to port 443. A URL without a path requests
`/` and saves the response as `index.html`. An explicit path saves under its
final path component. Raw spaces and control characters in a URL are rejected.

`-o FILE` / `--output=FILE` selects an output path for exactly one
URL. Existing files are never overwritten. Without `-o`, a collision selects
the first available name by appending `.1`, `.2`, and so on. Multiple URL
operands are processed sequentially. `-i FILE` / `--input-file=FILE` reads
one URL per line; blank lines and lines whose first nonspace character is `#`
are ignored. The URL list is fully read and validated before downloads begin.
`-q` / `--quiet` suppresses normal lifecycle messages, progress, and success
summaries but leaves errors visible. Standard grouped and attached option forms
are supported, and `--` ends option parsing.

HTTP and HTTPS response bodies are streamed to a temporary file and published
only after a complete response; failed transfers remove the incomplete
temporary file where filesystem certainty permits. Content-Length, chunked
transfer encoding, and connection-close-delimited bodies are supported.
Redirects (301, 302, 303, 307, and 308) are followed with relative-location
resolution, an eight-hop bound, and loop detection.

Normal output reports the requested URL, DNS resolution and the one IPv4
address currently returned by Mangrove's resolver, TCP connection, HTTP status,
redirects, response length and content type when present, destination name, and
a concise completion summary. For HTTPS, TCP success is reported before TLS
starts; the separate TLS 1.3 authenticated message appears only after mgTLS has
accepted the certificate chain, trusted certificate time, and hostname. Failed
TLS setup never reports authentication success. When the interactive terminal
capability is present, transfers taking longer than the update cadence show an
in-place byte/percentage and speed line. Known lengths show a percentage;
unknown lengths remain indeterminate. Redirected output receives no carriage
return progress bytes.

`--timings` adds monotonic DNS, TCP, TLS context, TLS handshake/authentication,
first-byte, response-transfer, receive-wait, file-write, temporary-file,
close, publication, and total measurements. Receive-wait and file-write costs
are separate; write timings include call count and maximum write duration.
DNS resolution, TCP connection, and TLS handshake have separate bounded waits
so a slow DNS response does not consume the TCP or TLS phase budget. HTTPS DNS
and TCP are measured separately from the TLS handshake. The receive timeout is an
inactivity limit, so a slow but progressing transfer can exceed it in total
duration. If the monotonic clock is unavailable, timing values are not claimed.

Current network drivers and protocol limits are described in
[network architecture](../network/architecture.md). A command's presence does
not imply support for every device or network configuration.

## Services and development programs

`/core/sprout`, `sessiond`, `logind`, `logd`, `networkd`, `deviced`, and
`volumed` are system services, not ordinary shell commands. `/bin/sprout` is
the service-control client; `/core/sprout` is the supervisor.

`fstest`, `fsstress`, `nettest`, and `hello` are development/test programs and
are not part of the normal image payload. MTS guest runners explicitly stage
the test programs they need. See the
[test-suite inventory](../development/test-suite.md).
