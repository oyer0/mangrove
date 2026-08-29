#!/usr/bin/env python3
import math
import os
import re
import struct
import sys

B = 4096
CRC_POLY = 0x42F0E1EBA9EA3693
MAGIC = b"MGFSv1\0\0"
VFS_UID_SYSTEM = 0
PERMISSION_OWNER_READ = 1 << 0
PERMISSION_OWNER_WRITE = 1 << 1
PERMISSION_OTHER_READ = 1 << 2
PERMISSION_OTHER_WRITE = 1 << 3
SYSTEM_PERMISSIONS = PERMISSION_OWNER_READ | PERMISSION_OWNER_WRITE | PERMISSION_OTHER_READ
USER_PERMISSIONS = PERMISSION_OWNER_READ | PERMISSION_OWNER_WRITE
RECORD_INLINE_DATA = 1
RECORD_OWNER_SHIFT = 1
RECORD_PERMISSIONS_SHIFT = 33

def w64(buf, off, value):
    struct.pack_into("<Q", buf, off, value)

def crc64(data):
    crc = 0
    for byte in data:
        crc ^= byte << 56
        for _ in range(8):
            crc = ((crc << 1) ^ CRC_POLY) & ((1 << 64) - 1) if crc & (1 << 63) else (crc << 1) & ((1 << 64) - 1)
    return crc

def checksum(buf, offset, length):
    w64(buf, offset, 0)
    w64(buf, offset, crc64(buf[:length]))

def extent(record, offset, logical, physical, count, flags=1):
    for value in (logical, physical, count, flags):
        w64(record, offset, value)
        offset += 8

def make_record(record_id, record_type, size=0, extents=(), owner_uid=VFS_UID_SYSTEM,
                permissions=SYSTEM_PERMISSIONS):
    record = bytearray(192)
    w64(record, 0, record_type)
    w64(record, 8, ((owner_uid << RECORD_OWNER_SHIFT) |
                    (permissions << RECORD_PERMISSIONS_SHIFT)))
    w64(record, 16, record_id)
    w64(record, 24, 1)
    w64(record, 32, size)
    w64(record, 40, len(extents))
    w64(record, 48, min(2, len(extents)))
    for index, item in enumerate(extents[:2]):
        extent(record, 64 + index * 32, *item)
    if len(extents) > 2:
        w64(record, 56, extents[2][1])
    checksum(record, 184, 192)
    return record

def record_payload_matches(image, table_start, record_id, payload):
    """Return whether a regular-file Record contains exactly payload."""
    slot = record_id - 1
    record_offset = (table_start + slot // 21) * B + 24 + (slot % 21) * 192
    if record_offset + 192 > len(image):
        return False
    record = image[record_offset:record_offset + 192]
    if struct.unpack_from("<Q", record, 0)[0] != 1:
        return False
    if struct.unpack_from("<Q", record, 32)[0] != len(payload):
        return False
    extent_count = struct.unpack_from("<Q", record, 40)[0]
    direct_count = min(2, extent_count)
    copied = bytearray()
    for index in range(direct_count):
        extent_offset = 64 + index * 32
        physical = struct.unpack_from("<Q", record, extent_offset + 8)[0]
        blocks = struct.unpack_from("<Q", record, extent_offset + 16)[0]
        if not blocks or physical * B + blocks * B > len(image):
            return False
        copied.extend(image[physical * B:physical * B + blocks * B])
    return bytes(copied[:len(payload)]) == payload

def directory_entry(record_id, name):
    encoded = name.encode("utf-8")
    size = (32 + len(encoded) + 7) & ~7
    entry = bytearray(size)
    w64(entry, 0, record_id)
    w64(entry, 8, len(encoded))
    w64(entry, 16, 1)
    entry[32:32 + len(encoded)] = encoded
    checksum(entry, 24, size)
    return entry

SYSTEM_FILES = (
    (8, "sprout"), (11, "shoot"), (10, "clear"), (13, "cp"),
    (14, "say"), (15, "uptime"), (12, "ls"), (16, "locate"),
    (21, "mv"), (22, "plant"), (23, "read"), (24, "rm"),
    (25, "version"), (26, "where"), (17, "ping"), (18, "resolve"),
    (19, "fetch"), (20, "network"), (27, "shutdown"), (28, "reboot"),
    (29, "power"), (32, "identity"), (37, "user"), (40, "mkdir"),
    (41, "rmdir"),
)

HELP_SOURCE_DIR = "share/help"
HELP_FILES = (
    "clear", "cp", "fetch", "identity", "locate", "ls", "mkdir", "mv",
    "network", "ping", "plant", "power", "read", "reboot", "resolve",
    "rm", "rmdir", "say", "shutdown", "uptime", "user", "version", "where",
)
HELP_INDEX_NAME = ".index"


def build_help_index(payloads, directory_size):
    entries = [b"version=1\ndirectory_size=%d\n" % directory_size]
    for name in HELP_FILES:
        fields = {}
        for raw_line in payloads[name].decode("utf-8").splitlines():
            line = raw_line.strip()
            if "=" not in line or line.startswith("//"):
                continue
            key, value = line.split("=", 1)
            if key in ("name", "category", "description"):
                fields[key] = value.strip()
        if set(fields) != {"name", "category", "description"}:
            raise SystemExit("populate_mgfs: invalid help metadata for %s" % name)
        entries.append(("name=%s\ncategory=%s\ndescription=%s\n\n" %
                        (fields["name"], fields["category"],
                         fields["description"])).encode("utf-8"))
    return b"\n".join(entries).rstrip() + b"\n"

NETWORK_RECORD_ID = 30
NETWORK_CONFIG_RECORD_ID = 31
DEVELOPER_HOME_RECORD_ID = 33
STATE_RECORD_ID = 34
ACCOUNTS_RECORD_ID = 35
ACCOUNT_DATABASE_RECORD_ID = 36
SESSION_RECORD_ID = 38
AUTOLOGIN_RECORD_ID = 39
SHARE_RECORD_ID = 42
HELP_RECORD_ID = 43
HELP_INDEX_RECORD_ID = 44
HELP_FIRST_RECORD_ID = 45
DEFAULT_NETWORK_CONFIG = (
    b"// Mangrove network configuration\n"
    b"\n"
    b"mode=dhcp\n"
)
DEFAULT_ACCOUNT_DATABASE = (
    b"version=2\n"
    b"next_uid=1001\n"
    b"\n"
    b"// Initial development identity\n"
    b"account uid=1000 username=developer role=admin "
    b"home=/user/developer flags=initial auth=none\n"
)


def main():
    if len(sys.argv) not in (27, 28):
        raise SystemExit("usage: populate_mgfs.py <image> "
                         "<sprout-elf> <shoot-elf> <clear-elf> <cp-elf> "
                         "<say-elf> <uptime-elf> <ls-elf> <locate-elf> "
                         "<mv-elf> <plant-elf> <read-elf> <rm-elf> "
                         "<version-elf> <where-elf> <ping-elf> <resolve-elf> "
                         "<fetch-elf> <network-elf> <shutdown-elf> "
                         "<reboot-elf> <power-elf> <identity-elf> "
                         "<user-elf> <mkdir-elf> <rmdir-elf> "
                         "[--autologin=<name>]")

    autologin = None
    if len(sys.argv) == 28:
        if not sys.argv[27].startswith("--autologin="):
            raise SystemExit("populate_mgfs: invalid image policy")
        autologin = sys.argv[27][len("--autologin="):]
        if not re.fullmatch(r"[a-z][a-z0-9_-]*", autologin):
            raise SystemExit("populate_mgfs: invalid autologin username")

    image_path = sys.argv[1]
    payloads = {
        record_id: open(path, "rb").read()
        for (record_id, _), path in zip(SYSTEM_FILES, sys.argv[2:27])
    }
    image = bytearray(open(image_path, "rb").read())
    if image[:8] != MAGIC:
        raise SystemExit("populate_mgfs: not an MGFS v1 image")

    total_blocks = struct.unpack_from("<Q", image, 40)[0]
    alloc_start = struct.unpack_from("<Q", image, 104)[0]
    record_bitmap_start = struct.unpack_from("<Q", image, 120)[0]
    table_start = struct.unpack_from("<Q", image, 136)[0]
    data_start = struct.unpack_from("<Q", image, 160)[0]
    record_count = struct.unpack_from("<Q", image, 152)[0]
    if table_start != 3 or data_start <= table_start or total_blocks * B != len(image):
        raise SystemExit("populate_mgfs: unsupported MGFS geometry")
    if max(record_id for record_id, _ in SYSTEM_FILES) >= record_count:
        raise SystemExit("populate_mgfs: image has insufficient record slots")

    root_directories = (
        (2, "bin"), (3, "boot"), (4, "core"),
        (5, "mount"), (6, "temp"), (7, "user"),
        (STATE_RECORD_ID, "state"), (SHARE_RECORD_ID, "share"),
    )
    root_data = b"".join(directory_entry(record_id, name)
                           for record_id, name in root_directories)
    bin_data = b"".join(directory_entry(record_id, name)
                         for record_id, name in SYSTEM_FILES)
    help_payloads = {
        name: open(os.path.join(HELP_SOURCE_DIR, name), "rb").read()
        for name in HELP_FILES
    }
    help_data = b"".join(
        directory_entry(HELP_FIRST_RECORD_ID + index, name)
        for index, name in enumerate(HELP_FILES))
    help_data = directory_entry(HELP_INDEX_RECORD_ID, HELP_INDEX_NAME) + help_data
    help_index_payload = build_help_index(help_payloads, len(help_data))
    core_data = (directory_entry(9, "sprout.txt") +
                 directory_entry(NETWORK_RECORD_ID, "network"))
    state_data = directory_entry(ACCOUNTS_RECORD_ID, "accounts")
    session_data = b""
    if autologin:
        core_data += directory_entry(SESSION_RECORD_ID, "session")
        session_data = directory_entry(AUTOLOGIN_RECORD_ID, "autologin")
    autologin_data = (autologin + "\n").encode("ascii") if autologin else b""
    network_data = directory_entry(NETWORK_CONFIG_RECORD_ID, "config")
    user_data = directory_entry(DEVELOPER_HOME_RECORD_ID, "developer")
    accounts_data = directory_entry(ACCOUNT_DATABASE_RECORD_ID, "users")
    share_data = directory_entry(HELP_RECORD_ID, "help")
    test_data = b"Mangrove handle I/O read succeeded\n"
    blocks_needed = 10 + (1 if autologin else 0) + \
        math.ceil(len(DEFAULT_NETWORK_CONFIG) / B) + \
        math.ceil(len(DEFAULT_ACCOUNT_DATABASE) / B) + \
        (math.ceil(len(autologin_data) / B) if autologin else 0) + \
        sum(math.ceil(len(payload) / B) for payload in payloads.values()) + \
        math.ceil(len(help_index_payload) / B) + \
        sum(math.ceil(len(payload) / B) for payload in help_payloads.values())
    first_block = data_start
    if first_block + blocks_needed >= total_blocks:
        raise SystemExit("populate_mgfs: image has insufficient data blocks")

    def write_block(block, data):
        image[block * B:(block + 1) * B] = data[:B].ljust(B, b"\0")

    write_block(first_block, root_data)
    write_block(first_block + 1, bin_data)
    write_block(first_block + 2, core_data)
    write_block(first_block + 3, state_data)
    write_block(first_block + 4, test_data)
    write_block(first_block + 5, network_data)
    write_block(first_block + 6, accounts_data)
    write_block(first_block + 7, user_data)
    write_block(first_block + 8, share_data)
    write_block(first_block + 9, help_data)

    file_extents = {}
    next_block = first_block + 10
    session_block = 0
    if autologin:
        session_block = next_block
        write_block(next_block, session_data)
        next_block += 1
    network_config_blocks = math.ceil(len(DEFAULT_NETWORK_CONFIG) / B)
    for index in range(network_config_blocks):
        write_block(next_block + index,
                    DEFAULT_NETWORK_CONFIG[index * B:(index + 1) * B])
    network_config_start = next_block
    next_block += network_config_blocks
    account_database_blocks = math.ceil(len(DEFAULT_ACCOUNT_DATABASE) / B)
    for index in range(account_database_blocks):
        write_block(next_block + index,
                    DEFAULT_ACCOUNT_DATABASE[index * B:(index + 1) * B])
    account_database_start = next_block
    next_block += account_database_blocks
    autologin_start = next_block
    if autologin:
        write_block(next_block, autologin_data)
        next_block += math.ceil(len(autologin_data) / B)
    for record_id, _ in SYSTEM_FILES:
        payload = payloads[record_id]
        block_count = math.ceil(len(payload) / B)
        for index in range(block_count):
            write_block(next_block + index, payload[index * B:(index + 1) * B])
        file_extents[record_id] = [(0, next_block, block_count, 1)]
        next_block += block_count
    help_extents = {}
    index_blocks = math.ceil(len(help_index_payload) / B)
    for block_index in range(index_blocks):
        write_block(next_block + block_index,
                    help_index_payload[block_index * B:(block_index + 1) * B])
    help_extents[HELP_INDEX_RECORD_ID] = [(0, next_block, index_blocks, 1)]
    next_block += index_blocks
    for index, name in enumerate(HELP_FILES):
        record_id = HELP_FIRST_RECORD_ID + index
        payload = help_payloads[name]
        block_count = math.ceil(len(payload) / B)
        for block_index in range(block_count):
            write_block(next_block + block_index,
                        payload[block_index * B:(block_index + 1) * B])
        help_extents[record_id] = [(0, next_block, block_count, 1)]
        next_block += block_count

    records = [
        make_record(1, 2, len(root_data), [(0, first_block, 1, 2)]),
        make_record(2, 2, len(bin_data), [(0, first_block + 1, 1, 2)]),
        make_record(3, 2),
        make_record(4, 2, len(core_data), [(0, first_block + 2, 1, 2)]),
        make_record(5, 2), make_record(6, 2),
        make_record(7, 2, len(user_data), [(0, first_block + 7, 1, 2)]),
        make_record(8, 1, len(payloads[8]), file_extents[8]),
        make_record(9, 1, len(test_data), [(0, first_block + 4, 1, 1)]),
    ]
    for record_id in range(10, max(record_id for record_id, _ in SYSTEM_FILES) + 1):
        if record_id in (NETWORK_RECORD_ID, NETWORK_CONFIG_RECORD_ID,
                         DEVELOPER_HOME_RECORD_ID, ACCOUNTS_RECORD_ID,
                         STATE_RECORD_ID, ACCOUNT_DATABASE_RECORD_ID,
                         SESSION_RECORD_ID, AUTOLOGIN_RECORD_ID):
            continue
        if record_id in payloads:
            records.append(make_record(record_id, 1, len(payloads[record_id]),
                                       file_extents[record_id]))
        else:
            records.append(make_record(record_id, 0))
    records.append(make_record(NETWORK_RECORD_ID, 2, len(network_data),
                               [(0, first_block + 5, 1, 2)]))
    records.append(make_record(NETWORK_CONFIG_RECORD_ID, 1,
                               len(DEFAULT_NETWORK_CONFIG),
                               [(0, network_config_start, network_config_blocks,
                                 1)]))
    records.append(make_record(DEVELOPER_HOME_RECORD_ID, 2,
                               owner_uid=1000,
                               permissions=USER_PERMISSIONS))
    records.append(make_record(STATE_RECORD_ID, 2, len(state_data),
                               [(0, first_block + 3, 1, 2)]))
    records.append(make_record(ACCOUNTS_RECORD_ID, 2, len(accounts_data),
                               [(0, first_block + 6, 1, 2)]))
    records.append(make_record(ACCOUNT_DATABASE_RECORD_ID, 1,
                               len(DEFAULT_ACCOUNT_DATABASE),
                               [(0, account_database_start,
                                 account_database_blocks, 1)]))
    records.append(make_record(SHARE_RECORD_ID, 2, len(share_data),
                               [(0, first_block + 8, 1, 2)]))
    records.append(make_record(HELP_RECORD_ID, 2, len(help_data),
                               [(0, first_block + 9, 1, 2)]))
    records.append(make_record(HELP_INDEX_RECORD_ID, 1,
                               len(help_index_payload),
                               help_extents[HELP_INDEX_RECORD_ID]))
    for index, name in enumerate(HELP_FILES):
        record_id = HELP_FIRST_RECORD_ID + index
        records.append(make_record(record_id, 1, len(help_payloads[name]),
                                   help_extents[record_id]))
    if autologin:
        records.append(make_record(SESSION_RECORD_ID, 2, len(session_data),
                                   [(0, session_block, 1, 2)]))
        records.append(make_record(AUTOLOGIN_RECORD_ID, 1,
                                   len(autologin_data),
                                   [(0, autologin_start,
                                     math.ceil(len(autologin_data) / B), 1)]))

    table_blocks = {}
    for slot, record in enumerate(records):
        block_index = slot // 21
        table = table_blocks.setdefault(
            block_index,
            bytearray(image[(table_start + block_index) * B:
                            (table_start + block_index + 1) * B]))
        table_offset = 24 + (slot % 21) * 192
        table[table_offset:table_offset + 192] = record
    for block_index, table in table_blocks.items():
        checksum(table, 16, B)
        image[(table_start + block_index) * B:
              (table_start + block_index + 1) * B] = table

    record_bitmap = bytearray(image[record_bitmap_start * B:(record_bitmap_start + 1) * B])
    for slot in range(len(records)):
        record_bitmap[24 + slot // 8] |= 1 << (slot % 8)
    checksum(record_bitmap, 16, B)
    image[record_bitmap_start * B:(record_bitmap_start + 1) * B] = record_bitmap

    allocation = bytearray(image[alloc_start * B:(alloc_start + 1) * B])
    for block in range(first_block, next_block):
        relative = block - data_start
        allocation[24 + relative // 8] |= 1 << (relative % 8)
    checksum(allocation, 16, B)
    image[alloc_start * B:(alloc_start + 1) * B] = allocation

    superblock = bytearray(image[:B])
    highest_record_id = max(record_id for record_id, _ in SYSTEM_FILES)
    if autologin:
        highest_record_id = max(highest_record_id, SESSION_RECORD_ID,
                                AUTOLOGIN_RECORD_ID)
    highest_record_id = max(highest_record_id, SHARE_RECORD_ID, HELP_RECORD_ID,
                            HELP_INDEX_RECORD_ID,
                            HELP_FIRST_RECORD_ID + len(HELP_FILES) - 1)
    w64(superblock, 96, highest_record_id + 1)
    checksum(superblock, 192, 200)
    image[:B] = superblock
    with open(image_path, "wb") as output:
        output.write(image)

if __name__ == "__main__":
    main()
