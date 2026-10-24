# Per-user application directories

Human accounts begin with an empty home directory. Mangrove does not install
application skeleton files or create hidden application directories during
account creation.

Applications may use these per-user paths when they need to persist data:

| Path | Purpose |
| --- | --- |
| `~/.config/` | User-selected application settings. |
| `~/.data/` | Persistent application-owned data. |
| `~/.state/` | History, sessions, and remembered application activity. |
| `~/.cache/` | Disposable data that can be regenerated. |

These are Mangrove conventions, not an XDG compatibility promise. System-wide
paths remain `/conf` for administrator-managed configuration, `/sys` for
persistent system-owned state, `/share` for shared resources, `/home` for
account homes, `/temp` for shared temporary files, and `/vol` for mounted
volumes.

Native applications use `userspace/common/app_directory.h`. The helper
resolves the current authenticated process identity and verifies that its
home is `/home/<username>` and owned by that identity. Applications provide a
validated lowercase application identifier, not a username or home path.
Path resolution is read-only. A separate explicit ensure operation creates
only the selected hidden root and application directory, and verifies their
ownership. Both operations use bounded native path lengths and return the
underlying filesystem error where possible.

The initial root names are fixed in shared userspace code. Users cannot
override them, and loam libc does not contain application-directory policy.
Applications should create a directory only when a real write needs it; a
missing optional settings file must not create configuration directories by
itself.
