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

## Shoot built-ins

These commands run inside Shoot and are not standalone `/bin` programs:
`jump` changes the working directory, `help` lists built-in commands,
`history` displays or manages the current session's history, `reload` reloads
Shoot configuration, and `exit` leaves the shell. See the
[history contract](../shell/history.md) for history options and persistence.

## Files and editing

| Command | Purpose |
| --- | --- |
| `list [ -l | --long ] [ -t | --tree DEPTH ] [path]` | List files; long metadata and bounded tree views are available. |
| `copy SOURCE... DESTINATION` | Copy one or more regular files. With multiple sources, the destination must be an existing directory and each file keeps its basename. Existing destinations are never overwritten. Directory sources are not copied. |
| `move SOURCE... DESTINATION` | Move or rename one or more files or directories. With multiple sources, the destination must be an existing directory and each source keeps its basename. Existing filesystem move safeguards apply; batches may partially succeed. |
| `plant NAME...` | Create one or more empty files. Existing files are never replaced; failures are reported per operand and the command returns failure if any target fails. |
| `del [-r|--recursive] TARGET...` | Remove each file or directory target in order. Directories are nonrecursive by default; `-r` explicitly removes trees. Failures are reported per target and later targets are still attempted; duplicates or targets already removed by an earlier recursive operand fail explicitly. |
| `type FILE` | Display a file. |
| `view FILE` | Open a regular text file in the read-only fullscreen viewer; see the [pager controls and limits](text-pager.md). |
| `info PROGRAM` | Read the installed fullscreen manual for one program from `/share/info/`; manuals are separate from quick help. |
| `locate NAME` | Show how a command name resolves. |
| `reed [FILE]` | Start the [Reed editor](../editor/README.md). |

del -r plans at most 4,096 entries and 32 directory levels before deleting
planned contents. Mount points, the current directory, and permission checks
remain protected by the kernel. Empty child directories may be removed during
preflight; failures are reported and return nonzero, but recursive removal is
not an atomic filesystem transaction.

`list` is Mangrove's directory-listing program, not a promise of GNU or POSIX
option compatibility. Pathnames use Mangrove's native byte-length and
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
| `shutdown`, `reboot` | Request an orderly system power-off or restart. Active applications and leaf services receive a bounded cleanup window; mounted filesystems are synchronized before the ACPI transition. A cleanup timeout or sync error cancels the request and reports failure. |

## Network

| Command | Purpose |
| --- | --- |
| `netinfo [status [INTERFACE]|interfaces|routes|neighbors|connections|dns]` | Inspect network state. |
| `netcfg COMMAND` | Configure automatic/manual addressing, enable/disable an interface, renew DHCP, or reload configuration. Mutations use networkd's authorization policy. |
| `ping [-c|--count COUNT] HOST` | Send ICMP echo requests. |
| `resolve HOSTNAME` | Resolve an IPv4 address. |
| `fetch URL` | Download an HTTP file to the current directory. |

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
