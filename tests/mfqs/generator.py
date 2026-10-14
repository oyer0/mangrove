# SPDX-License-Identifier: GPL-3.0-or-later
"""SplitMix64 lifecycle campaigns. Generation never reads production output."""
from model import Model, BACKENDS
from images import initial_files

MASK = (1 << 64) - 1
VECTORS = (0xe220a8397b1dcdaf, 0x6e789e6aa1b965f4,
           0x06c45d188009454f, 0xf88bb8a8724c81ec)


class Random:
    """SplitMix64, unsigned 64-bit wrap, high/low bits unchanged on all hosts.

    Selection uses rejection sampling, not platform random/hash ordering.
    Payload consumes one output per byte (low eight bits).
    """
    def __init__(self, seed):
        if type(seed) is not int or not 0 <= seed <= MASK:
            raise ValueError("seed must be an unsigned 64-bit integer")
        self.state = seed

    def next(self):
        self.state = (self.state + 0x9e3779b97f4a7c15) & MASK
        value = self.state
        value = ((value ^ (value >> 30)) * 0xbf58476d1ce4e5b9) & MASK
        value = ((value ^ (value >> 27)) * 0x94d049bb133111eb) & MASK
        return value ^ (value >> 31)

    def choose(self, values):
        bound = len(values)
        ceiling = (1 << 64) - ((1 << 64) % bound)
        while True:
            value = self.next()
            if value < ceiling:
                return values[value % bound]

    def payload(self, length):
        return bytes(self.next() & 255 for _ in range(length))


def generated_trace(backend, layout, seed, campaign="quick"):
    if campaign not in ("quick", "extended"):
        raise ValueError("unknown campaign")
    rng = Random(seed)
    model = Model(backend, initial_files(layout))
    trace = []
    boundaries = (0, 1, 511, 512, 513, 4095, 4096, 4097, 8191, 8192)

    def add(op, **fields):
        command = dict(op=op, **fields)
        model.apply(command)
        trace.append(command)
        if op in ("CREATE", "CREATE_ORDINARY", "MKDIR", "RMDIR", "UNLINK", "RENAME"):
            parents = {(fields["path"].rsplit("/", 1)[0] or "/")}
            if op == "RENAME":
                parents.add(fields["destination"].rsplit("/", 1)[0] or "/")
            for parent in sorted(parents):
                add("LIST", **({} if parent == "/" else dict(path=parent)))

    def verify():
        offset = model.offset
        size = len(model.files[model.opened])
        add("SEEK", offset=0)
        for at in range(0, size, 8192):
            add("READ", size=min(8192, size - at))
        add("READ", size=1)
        add("SEEK", offset=offset)

    def sweep(remount=False):
        assert model.opened is None
        if remount:
            add("REMOUNT")
        for directory in sorted(model.directories):
            add("LIST", **({} if directory == "/" else dict(path=directory)))
        for path in sorted(model.files):
            add("STAT", path=path)
            add("OPEN", path=path)
            verify()
            add("CLOSE")

    def check_file(path):
        add("OPEN", path=path)
        verify()
        add("CLOSE")
        add("STAT", path=path)

    add("HELLO")
    add("MKDIR", path="/box")
    add("MKDIR", path="/box/nest")
    # Mandatory lifecycle spine ensures every seed covers all advertised routes.
    add("CREATE_ORDINARY", path="/box/start.txt")
    add("WRITE", data=rng.payload(513).hex())
    verify()
    add("SEEK", offset=513)
    add("WRITE", data=rng.payload(1).hex())  # Explicit append.
    verify()
    add("WRITE", data="")
    verify()
    add("SEEK", offset=511)
    add("WRITE", data=rng.payload(2).hex())
    verify()
    add("TRUNCATE", size=0)
    verify()
    add("SEEK", offset=0)  # Resize never resets the handle's offset.
    add("WRITE", data=rng.payload(4097).hex())
    verify()
    add("RESIZE", size=512)  # Explicit UNSUPPORTED on MGFS/exFAT.
    verify()
    if backend == "fat32":
        add("RESIZE", size=4097)
        verify()
    add("CLOSE")
    add("RMDIR", path="/box")  # Nonempty directory refusal.
    add("CREATE", path="/box/start.txt")  # Exclusive collision, no handle.
    add("RENAME", path="/box/start.txt", destination="/box/nest/moved.txt")
    check_file("/box/nest/moved.txt")
    sweep(True)
    add("UNLINK", path="/box/nest/moved.txt")
    add("STAT", path="/box/nest/moved.txt")
    add("RMDIR", path="/box/nest")
    add("MKDIR", path="/box/nest")
    sweep()

    longest = "/" + "max-" + "x" * 251  # 255-byte component; not a Unicode claim.
    add("CREATE", path=longest)
    verify()
    add("WRITE", data=rng.payload(512).hex())
    verify()
    add("CLOSE")
    check_file(longest)
    add("UNLINK", path=longest)
    add("STAT", path=longest)
    # Interleave two allocations, grow the first while the second occupies
    # its adjacent storage, then reuse freed storage. No disk layout is mocked.
    for path in ("/frag1.dat", "/frag2.dat"):
        add("CREATE", path=path)
        add("WRITE", data=rng.payload(4096).hex())
        verify()
        add("CLOSE")
    add("OPEN", path="/frag1.dat")
    add("SEEK", offset=4096)
    add("WRITE", data=rng.payload(4096).hex())
    verify()
    add("CLOSE")
    add("UNLINK", path="/frag2.dat")
    add("CREATE_ORDINARY", path="/frag2.dat")
    add("WRITE", data=rng.payload(513).hex())
    verify()
    add("CLOSE")
    sweep(True)
    for path in ("/frag1.dat", "/frag2.dat"):
        add("UNLINK", path=path)
        add("STAT", path=path)
    slots = ("/f0", "/box/f1.txt", "/box/nest/f2", "/long-name-" + "a" * 245,
             "/box/" + "b" * 180, "/f5", "/box/f6", "/box/nest/f7")
    for step in range(16 if campaign == "quick" else 80):
        action = rng.choose(("create", "write", "append", "truncate", "rename", "unlink", "read"))
        files = sorted(model.files)
        unused = [p for p in slots if model.key(p) not in model.files]
        if not files or (action == "create" and unused):
            path = rng.choose(unused)
            add(rng.choose(("CREATE", "CREATE_ORDINARY")), path=path)
            add("WRITE", data=rng.payload(rng.choose(boundaries)).hex())
            verify()
            add("CLOSE")
            check_file(path)
        elif action == "unlink":
            path = rng.choose(files)
            add("UNLINK", path=path)
            add("STAT", path=path)
        elif action == "rename" and unused:
            path, target = rng.choose(files), rng.choose(unused)
            add("RENAME", path=path, destination=target)
            add("STAT", path=path)
            check_file(target)
        else:
            path = rng.choose(files)
            add("OPEN", path=path)
            size = len(model.files[model.opened])
            if action == "truncate":
                add("TRUNCATE", size=0)
            elif action in ("write", "append", "create", "rename"):
                limit = min(32768, size + (4097 if BACKENDS[backend]["gaps"] else 0))
                offset = size if action == "append" else rng.choose(tuple(sorted({0, size, *[b for b in boundaries if b <= limit]})))
                if action != "append":
                    offset = min(offset, 32768)
                add("SEEK", offset=offset)
                length = min(rng.choose(boundaries), 65536 - offset)
                add("WRITE", data=rng.payload(length).hex())
            else:
                add("SEEK", offset=rng.choose((0, size, min(65536, size + 1))))
                add("READ", size=rng.choose(boundaries))
            verify()
            add("CLOSE")
            check_file(path)
        # Check affected namespace after every mutating step, all contents
        # periodically and after real disposal/remount.
        if step % 8 == 7:
            sweep(step % 16 == 15)
    sweep(True)
    return trace
