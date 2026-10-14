# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent bytes/offset/namespace oracle. No driver or image parsing."""

BACKENDS = {
    "mgfs": dict(names="case-sensitive", gaps=False, resize="empty-only"),
    "fat32": dict(names="ascii-ci", gaps=True, resize="arbitrary"),
    "exfat": dict(names="ascii-ci", gaps=True, resize="empty-only"),
}


def capabilities(backend):
    policy = BACKENDS[backend]
    return (backend + ";create-exclusive-empty,create-ordinary,open,write,seek,read,stat,list,"
            "close,remount,truncate,resize,rename,unlink,mkdir,rmdir,snapshot,check,replace,create-contents;trusted-single-thread;MFQS1;" + policy["names"] + ";" +
            ("gap-zero" if policy["gaps"] else "no-sparse") + ";resize-" +
            policy["resize"]).encode("ascii")


class Model:
    def __init__(self, backend="mgfs", initial_files=None):
        self.backend = backend
        self.files = {self.key(path): bytes(data) for path, data in (initial_files or {}).items()}
        self.directories = {"/"}
        self.opened = None
        self.offset = 0
        self.save_current = True

    def key(self, path):
        # This profile permits only ASCII names, not generic Unicode fold.
        if BACKENDS[self.backend]["names"] == "case-sensitive":
            return path
        return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in path)

    def apply(self, command):
        op = command["op"]
        path = self.key(command["path"]) if "path" in command else None
        for field in ("path", "destination"):
            if field not in command: continue
            parts = self.key(command[field]).strip("/").split("/")
            if any("/" + "/".join(parts[:i]) in self.files for i in range(1, len(parts))):
                raise ValueError("a file used as a parent is outside the oracle domain")
        result = dict(status=0, size=0, offset=0, count=0, pins=0, data="")
        if op == "HELLO":
            result["data"] = capabilities(self.backend).hex()
        elif op in ("CREATE", "CREATE_ORDINARY", "CREATE_CONTENTS", "OPEN"):
            assert self.opened is None
            if op == "OPEN" and path in self.directories:
                raise ValueError("directory handles are outside the file model")
            if op != "OPEN" and (path in self.files or path in self.directories):
                result["status"] = -13  # Public VFS ALREADY_EXISTS ABI.
            elif ((path.rsplit("/", 1)[0] or "/") not in self.directories or
                  (op == "OPEN" and path not in self.files)):
                result["status"] = -2  # Public VFS NOT_FOUND ABI.
            else:
                if op != "OPEN":
                    self.files[path] = bytes.fromhex(command["data"]) if op == "CREATE_CONTENTS" else b""
                self.opened, self.offset = path, 0
                self.save_current = True
        elif op == "CLOSE":
            assert self.opened is not None
            self.opened, self.offset = None, 0
        elif op == "WRITE":
            assert self.opened is not None
            data = bytes.fromhex(command["data"])
            original = self.files[self.opened]
            if data and self.offset > len(original):
                assert BACKENDS[self.backend]["gaps"]
            end = self.offset + len(data)
            assert end <= 65536
            if data:
                self.save_current = False
                self.files[self.opened] = (original[:self.offset] +
                    bytes(max(0, self.offset - len(original))) + data + original[end:])
            self.offset = end
            result["count"] = len(data)
        elif op == "READ":
            assert self.opened is not None
            data = self.files[self.opened][self.offset:self.offset + command["size"]]
            self.offset += len(data)
            result.update(count=len(data), data=data.hex())
        elif op == "SEEK":
            assert self.opened is not None
            self.offset = command["offset"]
        elif op in ("TRUNCATE", "RESIZE"):
            assert self.opened is not None
            size = command["size"]
            if size and BACKENDS[self.backend]["resize"] != "arbitrary":
                result["status"] = -6
            else:
                old = self.files[self.opened]
                self.files[self.opened] = old[:size] + bytes(max(0, size - len(old)))
                # No-op version advancement is outside the portable contract.
                # A later generation assertion must first take a snapshot.
                self.save_current = False if size != len(old) else None
        elif op == "SNAPSHOT":
            assert self.opened is not None
            data = self.files[self.opened]
            self.save_current = True
            result.update(count=len(data), data=data.hex())
        elif op in ("CHECK", "REPLACE"):
            assert self.opened is not None
            if self.save_current is None:
                raise ValueError("snapshot required after a no-op resize before checking generation")
            if not self.save_current:
                result["status"] = -16
            elif op == "REPLACE":
                self.files[self.opened] = bytes.fromhex(command["data"])
                self.save_current = True
        elif op == "MKDIR":
            assert self.opened is None
            if path in self.files or path in self.directories:
                result["status"] = -13
            else:
                assert (path.rsplit("/", 1)[0] or "/") in self.directories
                self.directories.add(path)
        elif op in ("UNLINK", "RMDIR"):
            assert self.opened is None
            table = self.files if op == "UNLINK" else self.directories
            if (op == "UNLINK" and path in self.directories) or (op == "RMDIR" and path in self.files):
                raise ValueError("wrong-type retirement is outside the oracle domain")
            if path not in table:
                result["status"] = -2
            elif op == "RMDIR" and any(p.startswith(path + "/") for p in self.files.keys() | self.directories):
                result["status"] = -7
            elif op == "UNLINK":
                del self.files[path]
            else:
                self.directories.remove(path)
        elif op == "RENAME":
            assert self.opened is None
            target = self.key(command["destination"])
            if path not in self.files or target in self.files or target in self.directories:
                raise ValueError("only file moves to absent destinations are modeled")
            assert (target.rsplit("/", 1)[0] or "/") in self.directories
            self.files[target] = self.files.pop(path)
        elif op == "STAT":
            if path in self.directories:
                result.update(size=None, count=2)  # Directory size explicitly unqualified.
            elif path not in self.files:
                result["status"] = -2
            else:
                result.update(size=len(self.files[path]), count=1)  # VFS file type.
            return result
        elif op == "LIST":
            directory = path or "/"
            if directory not in self.directories:
                raise ValueError("LIST requires a modeled directory")
            names = sorted(p.rsplit("/", 1)[1].encode("ascii")
                           for p in self.files.keys() | (self.directories - {"/"})
                           if (p.rsplit("/", 1)[0] or "/") == directory)
            result.update(count=len(names), data=b"".join(n + b"\0" for n in names).hex())
            return result
        elif op == "REMOUNT":
            if self.opened is not None:
                result["status"] = -10  # Production unmount admission must refuse an open handle.
        else:
            raise ValueError("operation outside the qualified profile: " + op)
        if self.opened is not None:
            result.update(size=len(self.files[self.opened]), offset=self.offset)
        return result


def fixed_trace(backend="mgfs", initial_files=None):
    """No random generator. Boundary data comes from an independent formula."""
    trace = []
    model = Model(backend, initial_files)

    def add(op, **fields):
        command = dict(op=op, **fields)
        model.apply(command)
        trace.append(command)

    def verify_open():
        saved_offset = model.offset
        length = len(model.files[model.opened])
        add("SEEK", offset=0)
        for offset in range(0, length, 8192):
            add("READ", size=min(8192, length - offset))
        add("READ", size=1)  # Exact EOF, also covers empty files.
        add("SEEK", offset=saved_offset)

    add("HELLO")
    add("LIST")
    add("STAT", path="/missing")
    add("OPEN", path="/missing")
    if initial_files:
        add("OPEN", path="/old.txt")
        verify_open()
        for offset in (16, 4196):
            add("SEEK", offset=offset)
            add("WRITE", data="58")
            verify_open()
        add("CLOSE")
    add("CREATE", path="/alpha")
    verify_open()
    for data in (b"hello", bytes((i * 37 + 11) % 256 for i in range(4091))):
        add("WRITE", data=data.hex())
        verify_open()
    add("SEEK", offset=0)
    add("WRITE", data=b"Z".hex())  # Historical shrink-on-overwrite reproducer.
    verify_open()
    add("SEEK", offset=55)
    add("WRITE", data=b"middle".hex())
    verify_open()
    add("SEEK", offset=4095)
    add("WRITE", data=b"edge".hex())
    verify_open()
    add("SEEK", offset=4099)
    add("WRITE", data=bytes((i * 19 + 7) % 256 for i in range(4094)).hex())
    verify_open()
    add("WRITE", data="")
    verify_open()
    if BACKENDS[backend]["gaps"]:
        add("SEEK", offset=8209)
        add("WRITE", data="47")
        verify_open()
    add("REMOUNT")  # Must refuse with the file handle open.
    add("CLOSE")
    add("CREATE", path="/alpha")  # Exclusive namespace conflict, no new handle.
    if backend != "mgfs":
        add("CREATE", path="/ALPHA")
        add("OPEN", path="/AlPhA")
        verify_open()
        add("CLOSE")
    add("STAT", path="/alpha")
    add("LIST")
    add("OPEN", path="/alpha")
    verify_open()
    add("CLOSE")
    add("CREATE", path="/empty")
    verify_open()
    add("CLOSE")
    if backend != "mgfs":
        add("CREATE_ORDINARY", path="/plain")
        add("WRITE", data="6f7264696e617279")
        verify_open()
        add("CLOSE")
        add("CREATE_ORDINARY", path="/PLAIN")
    for _ in range(2):
        add("REMOUNT")
        add("LIST")
        for path in tuple(model.files):
            add("STAT", path=path)
            add("OPEN", path=path)
            verify_open()
            add("CLOSE")
    return trace
