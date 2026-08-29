#!/usr/bin/env python3
"""Update Mangrove's fixed system payloads without rebuilding user data."""

import math
import os
import re
import struct
import sys

from populate_mgfs import (B, DEFAULT_ACCOUNT_DATABASE,
                           DEFAULT_NETWORK_CONFIG, MAGIC, checksum,
                           PERMISSION_OWNER_READ, PERMISSION_OWNER_WRITE,
                           PERMISSION_OTHER_READ, PERMISSION_OTHER_WRITE,
                           SYSTEM_PERMISSIONS, USER_PERMISSIONS,
                           RECORD_INLINE_DATA, RECORD_OWNER_SHIFT,
                           RECORD_PERMISSIONS_SHIFT, directory_entry, w64)
from populate_mgfs import (HELP_FILES, HELP_INDEX_NAME, build_help_index)

EXTENT_LIST_MAGIC = 0x315458455346474D
EXTENTS_PER_LIST_BLOCK = 126
RECORD_BYTES = 192
RECORDS_PER_TABLE_BLOCK = 21
METADATA_CHECKSUM_OFFSET = 16
RECORD_CHECKSUM_OFFSET = 184
SUPER_CHECKSUM_OFFSET = 192
MGFS_FORMAT_MINOR = 1
VFS_UID_SYSTEM = 0
DEVELOPER_UID = 1000
FIRST_USER_UID = 1001
ACCOUNT_MAX_RECORDS = 64
IDENTITY_USERNAME_CAPACITY = 32
IDENTITY_HOME_CAPACITY = 256
PASSWORD_SALT_BYTES = 16
PASSWORD_HASH_BYTES = 32
PASSWORD_MIN_ITERATIONS = 10000
PASSWORD_MAX_ITERATIONS = 1000000

SYSTEM_RECORDS = {
    8: "sprout",
    11: "shoot",
    10: "clear",
    13: "cp",
    14: "say",
    15: "uptime",
    12: "ls",
    16: "locate",
    21: "mv",
    22: "plant",
    23: "read",
    24: "rm",
    25: "version",
    26: "where",
    17: "ping",
    18: "resolve",
    19: "fetch",
    20: "network",
    27: "shutdown",
    28: "reboot",
    29: "power",
    32: "identity",
    37: "user",
}

NETWORK_RECORD_ID = 30
NETWORK_CONFIG_RECORD_ID = 31
DEVELOPER_HOME_RECORD_ID = 33
STATE_RECORD_ID = 34
ACCOUNTS_RECORD_ID = 35
ACCOUNT_DATABASE_RECORD_ID = 36
LEGACY_ACCOUNT_DATABASE_NAME = "users.db"
ACCOUNT_DATABASE_NAME = "users"
SESSION_RECORD_ID = 38
AUTOLOGIN_RECORD_ID = 39
SHARE_RECORD_ID = 42
HELP_RECORD_ID = 43
HELP_FIRST_RECORD_ID = 44
HELP_SOURCE_DIR = "share/help"
HELP_NAMES = (HELP_INDEX_NAME,) + HELP_FILES
RESERVED_SYSTEM_RECORD_IDS = frozenset(SYSTEM_RECORDS) | {
    NETWORK_RECORD_ID, NETWORK_CONFIG_RECORD_ID, DEVELOPER_HOME_RECORD_ID,
    STATE_RECORD_ID, ACCOUNTS_RECORD_ID, ACCOUNT_DATABASE_RECORD_ID,
}

# This order is the public payload argument order used by make_image.sh.
# Keep it explicit: record IDs are not numerically ordered because the
# directory metadata occupies the intervening records.
PAYLOAD_RECORD_IDS = (8, 11, 10, 13, 14, 15, 12, 16, 21, 22, 23, 24,
                      25, 26, 17, 18, 19, 20, 27, 28, 29, 32, 37,
                      None, None)
PAYLOAD_NAMES = (
    "sprout", "shoot", "clear", "cp", "say", "uptime", "ls", "locate",
    "mv", "plant", "read", "rm", "version", "where", "ping", "resolve",
    "fetch", "network", "shutdown", "reboot", "power", "identity", "user",
    "mkdir", "rmdir",
)

NETWORK_NAME = "network"
NETWORK_CONFIG_NAME = "config"


def _account_strip_comment(line):
    quoted = False
    index = 0
    while index + 1 < len(line):
        if line[index] == '"':
            quoted = not quoted
        if not quoted and line[index:index + 2] == "//":
            return line[:index].rstrip()
        index += 1
    return line.rstrip()


def _account_parse_uid(value):
    if not value or not value.isdigit():
        raise RuntimeError("account database has invalid UID")
    uid = int(value, 10)
    if uid > 0xffffffff or uid == VFS_UID_SYSTEM:
        raise RuntimeError("account database has reserved UID")
    return uid


def _account_parse_records(lines, legacy, version):
    records = []
    names = set()
    uids = set()
    initial_count = 0
    username_pattern = re.compile(r"^[a-z][a-z0-9_-]*$")
    for line in lines:
        line = _account_strip_comment(line).strip()
        if not line:
            continue
        tokens = line.split()
        if not tokens or tokens[0] != "account":
            raise RuntimeError("account database has malformed record")
        values = {}
        for token in tokens[1:]:
            if "=" not in token:
                raise RuntimeError("account database has malformed record")
            key, value = token.split("=", 1)
            valid_keys = {"uid", "username", "role", "home", "flags"}
            if version == 2:
                valid_keys |= {"auth", "salt", "iterations", "hash"}
            if key in values or key not in valid_keys or not value:
                raise RuntimeError("account database has malformed record")
            values[key] = value
        required = {"uid", "username", "role", "home", "flags"}
        if version == 2:
            required.add("auth")
        if not required.issubset(values):
            raise RuntimeError("account database has incomplete record")
        uid = _account_parse_uid(values["uid"])
        username = values["username"]
        home = values["home"]
        if len(username) >= IDENTITY_USERNAME_CAPACITY or not username_pattern.fullmatch(username):
            raise RuntimeError("account database has invalid username")
        if values["role"] not in {"regular", "admin"}:
            raise RuntimeError("account database has invalid role")
        expected_home = "/user/" + username
        if legacy and home == "/home/" + username:
            home = expected_home
        if home != expected_home or len(home) >= IDENTITY_HOME_CAPACITY:
            raise RuntimeError("account database has invalid home")
        if legacy:
            flag_tokens = values["flags"].split(",")
            if "disabled" in flag_tokens or any(flag not in {"enabled", "initial"} for flag in flag_tokens):
                raise RuntimeError("account database has unsupported account state")
            flags = "initial" if "initial" in flag_tokens else "none"
        else:
            if values["flags"] not in {"none", "initial"}:
                raise RuntimeError("account database has invalid account flags")
            flags = values["flags"]
        if uid in uids or username in names:
            raise RuntimeError("account database has duplicate account")
        uids.add(uid)
        names.add(username)
        if flags == "initial":
            initial_count += 1
        if version == 1:
            authentication = ("none", "", 0, "")
        elif values["auth"] == "none":
            if any(key in values for key in ("salt", "iterations", "hash")):
                raise RuntimeError("account database has invalid auth metadata")
            authentication = ("none", "", 0, "")
        elif values["auth"] == "pbkdf2-sha256":
            if set(values) != required | {"salt", "iterations", "hash"}:
                raise RuntimeError("account database has incomplete auth metadata")
            salt = values["salt"]
            digest = values["hash"]
            if (len(salt) != PASSWORD_SALT_BYTES * 2 or
                    len(digest) != PASSWORD_HASH_BYTES * 2):
                raise RuntimeError("account database has invalid auth metadata")
            try:
                bytes.fromhex(salt)
                bytes.fromhex(digest)
            except ValueError:
                raise RuntimeError("account database has invalid auth metadata")
            if not values["iterations"].isdigit():
                raise RuntimeError("account database has invalid auth iterations")
            iterations = int(values["iterations"], 10)
            if not PASSWORD_MIN_ITERATIONS <= iterations <= PASSWORD_MAX_ITERATIONS:
                raise RuntimeError("account database has invalid auth iterations")
            authentication = ("pbkdf2-sha256", salt.lower(), iterations,
                              digest.lower())
        else:
            raise RuntimeError("account database has unknown auth algorithm")
        records.append((uid, username, values["role"], home, flags,
                        authentication))
    if not records or initial_count != 1:
        raise RuntimeError("account database must contain one initial account")
    return records


def parse_account_database(payload):
    if len(payload) == 0 or len(payload) > 16384:
        raise RuntimeError("account database has invalid size")
    try:
        text = payload.decode("ascii")
    except UnicodeDecodeError:
        raise RuntimeError("account database is not ASCII")
    lines = text.splitlines()
    meaningful = []
    for line in lines:
        stripped = _account_strip_comment(line).strip()
        if stripped:
            meaningful.append(stripped)
    if not meaningful:
        raise RuntimeError("account database is empty")
    first = meaningful[0]
    if first == "MangroveAccounts 1":
        records = _account_parse_records(meaningful[1:], True, 1)
        legacy = True
        next_uid = max(FIRST_USER_UID, max(record[0] for record in records) + 1)
    elif first == "version=1":
        if len(meaningful) < 2 or not meaningful[1].startswith("next_uid="):
            raise RuntimeError("account database has no next_uid")
        next_text = meaningful[1][len("next_uid="):]
        if not next_text.isdigit():
            raise RuntimeError("account database has invalid next_uid")
        next_uid = int(next_text, 10)
        if next_uid < FIRST_USER_UID or next_uid >= 0x100000000:
            raise RuntimeError("account database has invalid next_uid")
        records = _account_parse_records(meaningful[2:], False, 1)
        legacy = False
    elif first == "version=2":
        if len(meaningful) < 2 or not meaningful[1].startswith("next_uid="):
            raise RuntimeError("account database has no next_uid")
        next_text = meaningful[1][len("next_uid="):]
        if not next_text.isdigit():
            raise RuntimeError("account database has invalid next_uid")
        next_uid = int(next_text, 10)
        if next_uid < FIRST_USER_UID or next_uid >= 0x100000000:
            raise RuntimeError("account database has invalid next_uid")
        records = _account_parse_records(meaningful[2:], False, 2)
        legacy = False
    else:
        raise RuntimeError("unsupported account database version")
    if next_uid <= max(record[0] for record in records):
        raise RuntimeError("account database next_uid is not monotonic")
    return records, next_uid, legacy


def serialize_account_database(records, next_uid):
    output = ["version=2", "next_uid=%d" % next_uid, ""]
    for uid, username, role, home, flags, authentication in records:
        algorithm, salt, iterations, digest = authentication
        line = ("account uid=%d username=%s role=%s home=%s flags=%s auth=%s" %
                (uid, username, role, home, flags, algorithm))
        if algorithm == "pbkdf2-sha256":
            line += " salt=%s iterations=%d hash=%s" % (
                salt, iterations, digest)
        output.append(line)
    payload = ("\n".join(output) + "\n").encode("ascii")
    if len(payload) > 16384:
        raise RuntimeError("account database exceeds maximum size")
    return payload


def migrate_account_database(payload):
    records, next_uid, legacy = parse_account_database(payload)
    converted = serialize_account_database(records, next_uid)
    return converted, legacy or converted != payload


def u64(buf, offset):
    return struct.unpack_from("<Q", buf, offset)[0]


def record_offset(image, table_start, record_id, record_count):
    for slot in range(record_count):
        table_block = slot // RECORDS_PER_TABLE_BLOCK
        table_slot = slot % RECORDS_PER_TABLE_BLOCK
        offset = (table_start + table_block) * B + 24 + table_slot * RECORD_BYTES
        if u64(image, offset + 16) == record_id:
            return offset
    if 0 < record_id <= record_count:
        slot = record_id - 1
        table_block = slot // RECORDS_PER_TABLE_BLOCK
        table_slot = slot % RECORDS_PER_TABLE_BLOCK
        return (table_start + table_block) * B + 24 + table_slot * RECORD_BYTES
    raise RuntimeError("system Record ID %d is outside the record table" % record_id)


def record_slot_from_offset(offset, table_start):
    relative = offset - table_start * B
    offset_in_block = relative % B
    if relative < 0 or offset_in_block < 24 or \
            (offset_in_block - 24) % RECORD_BYTES:
        raise RuntimeError("invalid MGFS Record table offset")
    table_block = relative // B
    slot_in_block = (offset_in_block - 24) // RECORD_BYTES
    if slot_in_block >= RECORDS_PER_TABLE_BLOCK:
        raise RuntimeError("invalid MGFS Record table slot")
    return table_block * RECORDS_PER_TABLE_BLOCK + slot_in_block


def record_table_offset(layout, slot):
    return (layout["record_table_start"] + slot // RECORDS_PER_TABLE_BLOCK) * B + \
        24 + (slot % RECORDS_PER_TABLE_BLOCK) * RECORD_BYTES


def record_slot_allocated(image, layout, slot):
    offset = layout["record_bitmap_start"] * B + 24 + slot // 8
    return bool(image[offset] & (1 << (slot % 8)))


def set_record_allocated(image, layout, slot, value):
    offset = layout["record_bitmap_start"] * B + 24 + slot // 8
    mask = 1 << (slot % 8)
    if value:
        image[offset] |= mask
    else:
        image[offset] &= ~mask & 0xff


def all_record_ids(image, layout):
    result = set()
    for slot in range(layout["record_count"]):
        offset = record_table_offset(layout, slot)
        record_id = u64(image, offset + 16)
        if record_id:
            result.add(record_id)
    return result


def allocate_record_slots(image, layout, count, excluded_slots=()):
    excluded = set(excluded_slots)
    slots = []
    for slot in range(layout["record_count"]):
        if slot in excluded or record_slot_allocated(image, layout, slot):
            continue
        set_record_allocated(image, layout, slot, True)
        slots.append(slot)
        if len(slots) == count:
            return slots
    raise RuntimeError("MGFS Record table is full while adding network config")


def directory_entries(payload):
    entries = []
    offset = 0
    while offset < len(payload):
        if len(payload) - offset < 32:
            raise RuntimeError("truncated MGFS directory entry")
        name_length = u64(payload, offset + 8)
        entry_size = (32 + name_length + 7) & ~7
        if not name_length or entry_size > len(payload) - offset:
            raise RuntimeError("invalid MGFS directory entry")
        name = bytes(payload[offset + 32:offset + 32 + name_length])
        entries.append((offset, entry_size, u64(payload, offset),
                        u64(payload, offset + 16), name))
        offset += entry_size
    return entries


def directory_find(payload, name):
    wanted = name.encode("utf-8")
    for offset, size, record_id, flags, entry_name in directory_entries(payload):
        if flags == 1 and entry_name == wanted:
            return record_id
    return None


def directory_rename(payload, old_name, new_name):
    """Rename one live directory entry without changing its Record ID."""
    old_bytes = old_name.encode("utf-8")
    new_bytes = new_name.encode("utf-8")
    old_entry = None
    for offset, size, record_id, flags, name in directory_entries(payload):
        if flags != 1:
            continue
        if name == new_bytes:
            raise RuntimeError("directory already contains %s" % new_name)
        if name == old_bytes:
            if old_entry is not None:
                raise RuntimeError("directory contains duplicate %s" % old_name)
            old_entry = (offset, size, record_id)
    if old_entry is None:
        raise RuntimeError("directory has no %s entry" % old_name)
    replacement = directory_entry(old_entry[2], new_name)
    if len(replacement) != old_entry[1]:
        raise RuntimeError("directory entry rename changes its allocated size")
    result = bytearray(payload)
    result[old_entry[0]:old_entry[0] + old_entry[1]] = replacement
    return bytes(result)


def directory_remove(payload, name):
    """Remove one live entry while preserving all other directory bytes."""
    wanted = name.encode("utf-8")
    result = bytearray()
    found = False
    for offset, size, record_id, flags, entry_name in directory_entries(payload):
        if flags == 1 and entry_name == wanted:
            if found:
                raise RuntimeError("directory contains duplicate %s" % name)
            found = True
            continue
        result.extend(payload[offset:offset + size])
    if not found:
        raise RuntimeError("directory has no %s entry" % name)
    return bytes(result)


def extent_values(record, offset):
    return (u64(record, offset), u64(record, offset + 8),
            u64(record, offset + 16), u64(record, offset + 24))


def record_extents(image, record):
    extent_count = u64(record, 40)
    inline_count = u64(record, 48)
    list_block = u64(record, 56)
    extents = []
    list_blocks = []

    for index in range(inline_count):
        extents.append(extent_values(record, 64 + index * 32))

    while list_block:
        if list_block in list_blocks:
            raise RuntimeError("cyclic system extent-list chain")
        list_blocks.append(list_block)
        offset = list_block * B
        if u64(image, offset) != EXTENT_LIST_MAGIC:
            raise RuntimeError("invalid system extent-list block")
        entry_count = u64(image, offset + 24)
        if not entry_count or entry_count > EXTENTS_PER_LIST_BLOCK:
            raise RuntimeError("invalid system extent-list entry count")
        for index in range(entry_count):
            extents.append(extent_values(image, offset + 64 + index * 32))
        list_block = u64(image, offset + 16)

    if len(extents) != extent_count:
        raise RuntimeError("incomplete system extent-list chain")
    return extents, list_blocks


def record_payload(image, record):
    size = u64(record, 32)
    extents, _ = record_extents(image, record)
    payload = bytearray()
    for _, physical, count, flags in extents:
        if flags not in (1, 2):
            raise RuntimeError("invalid system extent")
        for block in range(physical, physical + count):
            offset = block * B
            payload.extend(image[offset:offset + B])
    return bytes(payload[:size])


def record_extent_layout_valid(image, layout, record):
    """Validate the logical ordering used by a Record's extent chain."""
    try:
        extents, list_blocks = record_extents(image, record)
    except (IndexError, RuntimeError, struct.error):
        return False
    expected_logical = 0
    for logical, physical, count, flags in extents:
        if logical != expected_logical or count == 0 or flags not in (1, 2):
            return False
        if physical < layout["data_start"] or \
                physical + count > layout["data_start"] + layout["data_blocks"]:
            return False
        expected_logical += count
    for block in list_blocks:
        if block < layout["data_start"] or \
                block >= layout["data_start"] + layout["data_blocks"]:
            return False
    required_blocks = (u64(record, 32) + B - 1) // B
    return expected_logical >= required_blocks


def record_owner(record):
    return (u64(record, 8) >> RECORD_OWNER_SHIFT) & 0xffffffff


def record_permissions(record):
    return (u64(record, 8) >> RECORD_PERMISSIONS_SHIFT) & 0xf


def bitmap_location(layout, bit):
    bitmap_block = bit // 32576
    byte_offset = bitmap_block * B + 24 + (bit % 32576) // 8
    return layout["allocation_bitmap_start"] * B + byte_offset, 1 << (bit % 8)


def set_allocated(image, layout, physical_block, value):
    data_start = layout["data_start"]
    data_blocks = layout["data_blocks"]
    if physical_block < data_start or physical_block >= data_start + data_blocks:
        raise RuntimeError("system block lies outside the data area")
    offset, mask = bitmap_location(layout, physical_block - data_start)
    if value:
        image[offset] |= mask
    else:
        image[offset] &= ~mask & 0xff


def is_allocated(image, layout, physical_block):
    offset, mask = bitmap_location(layout, physical_block - layout["data_start"])
    return bool(image[offset] & mask)


def allocate_blocks(image, layout, count, reserved):
    blocks = []
    if count == 0:
        return blocks
    for physical in range(layout["data_start"],
                          layout["data_start"] + layout["data_blocks"]):
        if physical in reserved or is_allocated(image, layout, physical):
            continue
        set_allocated(image, layout, physical, True)
        reserved.add(physical)
        blocks.append(physical)
        if len(blocks) == count:
            return blocks
    raise RuntimeError("MGFS data area is full while updating system files")


def make_extents(blocks, flags=1):
    if not blocks:
        return []
    extents = []
    start = previous = blocks[0]
    logical = 0
    for block in blocks[1:]:
        if block == previous + 1:
            previous = block
            continue
        count = previous - start + 1
        extents.append((logical, start, count, flags))
        logical += count
        start = previous = block
    count = previous - start + 1
    extents.append((logical, start, count, flags))
    return extents


def record_flags(owner_uid=VFS_UID_SYSTEM, permissions=SYSTEM_PERMISSIONS,
                 inline_data=False):
    return ((RECORD_INLINE_DATA if inline_data else 0) |
            (owner_uid << RECORD_OWNER_SHIFT) |
            (permissions << RECORD_PERMISSIONS_SHIFT))


def write_record(image, offset, record_id, record_type, generation, payload,
                 extents, list_head, owner_uid=VFS_UID_SYSTEM,
                 permissions=SYSTEM_PERMISSIONS):
    record = bytearray(RECORD_BYTES)
    w64(record, 0, record_type)
    w64(record, 8, record_flags(owner_uid, permissions,
                               record_type == 1 and not extents))
    w64(record, 16, record_id)
    w64(record, 24, generation)
    w64(record, 32, len(payload))
    w64(record, 40, len(extents))
    w64(record, 48, min(2, len(extents)))
    w64(record, 56, list_head)
    for index, extent in enumerate(extents[:2]):
        for value_index, value in enumerate(extent):
            w64(record, 64 + index * 32 + value_index * 8, value)
    checksum(record, RECORD_CHECKSUM_OFFSET, RECORD_BYTES)
    image[offset:offset + RECORD_BYTES] = record


def write_extent_lists(image, record_id, extents, list_blocks):
    remaining = extents[2:]
    for index, physical in enumerate(list_blocks):
        block = bytearray(B)
        w64(block, 0, EXTENT_LIST_MAGIC)
        w64(block, 8, record_id)
        w64(block, 16, list_blocks[index + 1] if index + 1 < len(list_blocks) else 0)
        entries = remaining[index * EXTENTS_PER_LIST_BLOCK:
                           (index + 1) * EXTENTS_PER_LIST_BLOCK]
        w64(block, 24, len(entries))
        for entry_index, extent in enumerate(entries):
            for value_index, value in enumerate(extent):
                w64(block, 64 + entry_index * 32 + value_index * 8, value)
        checksum(block, 32, B)
        offset = physical * B
        image[offset:offset + B] = block


def replace_directory_payload(image, layout, record_id, payload, reserved):
    """Replace a directory payload while retaining its existing entries."""
    offset = record_offset(image, layout["record_table_start"], record_id,
                           layout["record_count"])
    old_record = bytes(image[offset:offset + RECORD_BYTES])
    old_extents, old_list_blocks = record_extents(image, old_record)
    old_data_blocks = []
    for _, physical, count, flags in old_extents:
        if flags != 2:
            raise RuntimeError("directory Record has non-directory extents")
        old_data_blocks.extend(range(physical, physical + count))

    needed = math.ceil(len(payload) / B)
    data_blocks = old_data_blocks[:]
    if len(data_blocks) < needed:
        data_blocks.extend(allocate_blocks(image, layout,
                                            needed - len(data_blocks), reserved))
    selected_blocks = data_blocks[:needed]
    extents = make_extents(selected_blocks, 2)
    list_count = math.ceil(max(0, len(extents) - 2) / EXTENTS_PER_LIST_BLOCK)
    list_blocks = allocate_blocks(image, layout, list_count, reserved)

    for index, block in enumerate(selected_blocks):
        start = index * B
        image[block * B:(block + 1) * B] = payload[start:start + B].ljust(B, b"\0")
    write_extent_lists(image, record_id, extents, list_blocks)
    write_record(image, offset, record_id, 2, u64(old_record, 24) + 1,
                 payload, extents, list_blocks[0] if list_blocks else 0,
                 record_owner(old_record), record_permissions(old_record))

    for block in old_data_blocks[len(selected_blocks):]:
        set_allocated(image, layout, block, False)
    for block in old_list_blocks:
        if block not in list_blocks:
            set_allocated(image, layout, block, False)
    return record_slot_from_offset(offset, layout["record_table_start"]) // \
        RECORDS_PER_TABLE_BLOCK


def replace_file_payload(image, layout, record_id, payload, reserved):
    """Replace a file payload while retaining its ownership and Record ID."""
    offset = record_offset(image, layout["record_table_start"], record_id,
                           layout["record_count"])
    old_record = bytes(image[offset:offset + RECORD_BYTES])
    if u64(old_record, 0) != 1:
        raise RuntimeError("account database is not a file")
    old_extents, old_list_blocks = record_extents(image, old_record)
    old_data_blocks = []
    for _, physical, count, flags in old_extents:
        if flags != 1:
            raise RuntimeError("account database has invalid data extents")
        old_data_blocks.extend(range(physical, physical + count))

    needed = math.ceil(len(payload) / B)
    data_blocks = allocate_blocks(image, layout, needed, reserved)
    extents = make_extents(data_blocks, 1)
    list_count = math.ceil(max(0, len(extents) - 2) / EXTENTS_PER_LIST_BLOCK)
    list_blocks = allocate_blocks(image, layout, list_count, reserved)
    for index, block in enumerate(data_blocks):
        start = index * B
        image[block * B:(block + 1) * B] = payload[start:start + B].ljust(B, b"\0")
    write_extent_lists(image, record_id, extents, list_blocks)
    write_record(image, offset, record_id, 1, u64(old_record, 24) + 1,
                 payload, extents, list_blocks[0] if list_blocks else 0,
                 record_owner(old_record), record_permissions(old_record))

    for block in old_data_blocks:
        set_allocated(image, layout, block, False)
    for block in old_list_blocks:
        set_allocated(image, layout, block, False)
    return record_slot_from_offset(offset, layout["record_table_start"]) // \
        RECORDS_PER_TABLE_BLOCK


def refresh_table_checksum(image, layout, table_index):
    offset = (layout["record_table_start"] + table_index) * B
    block = bytearray(image[offset:offset + B])
    checksum(block, METADATA_CHECKSUM_OFFSET, B)
    image[offset:offset + B] = block


def refresh_record_bitmap_checksum(image, layout):
    for index in range(layout["record_bitmap_blocks"]):
        offset = (layout["record_bitmap_start"] + index) * B
        block = bytearray(image[offset:offset + B])
        checksum(block, METADATA_CHECKSUM_OFFSET, B)
        image[offset:offset + B] = block


def ensure_network_config(image, layout):
    """Add the default persistent config only to legacy images missing it."""
    root_offset = record_offset(image, layout["record_table_start"], 1,
                                layout["record_count"])
    root_payload = record_payload(image, bytes(image[root_offset:
                                                    root_offset + RECORD_BYTES]))
    core_id = directory_find(root_payload, "core")
    if core_id is None:
        raise RuntimeError("root directory has no core directory")
    core_offset = record_offset(image, layout["record_table_start"], core_id,
                                layout["record_count"])
    core_record = bytes(image[core_offset:core_offset + RECORD_BYTES])
    core_payload = record_payload(image, core_record)
    network_id = directory_find(core_payload, NETWORK_NAME)
    if network_id is not None:
        network_offset = record_offset(image, layout["record_table_start"],
                                       network_id, layout["record_count"])
        network_record = bytes(image[network_offset:network_offset + RECORD_BYTES])
        if u64(network_record, 0) != 2:
            raise RuntimeError("core/network is not a directory")
        network_payload = record_payload(image, network_record)
    else:
        network_payload = b""

    config_id = directory_find(network_payload, NETWORK_CONFIG_NAME)
    if config_id is not None:
        config_offset = record_offset(image, layout["record_table_start"],
                                      config_id, layout["record_count"])
        config_record = bytes(image[config_offset:config_offset + RECORD_BYTES])
        if u64(config_record, 0) != 1:
            raise RuntimeError("core/network/config is not a file")
        return False

    used_ids = all_record_ids(image, layout)
    next_id = max(2, u64(image, 96), max(used_ids, default=1) + 1)
    reserved_slots = {record_id - 1 for record_id in
                      RESERVED_SYSTEM_RECORD_IDS
                      if 0 < record_id <= layout["record_count"]}
    slots = allocate_record_slots(
        image, layout, 1 if network_id is not None else 2, reserved_slots)
    slot_index = 0

    if network_id is None:
        while next_id in used_ids or next_id in RESERVED_SYSTEM_RECORD_IDS:
            next_id += 1
        network_id = next_id
        next_id += 1
        used_ids.add(network_id)
        network_slot = slots[slot_index]
        slot_index += 1
    else:
        network_slot = None

    while next_id in used_ids or next_id in RESERVED_SYSTEM_RECORD_IDS:
        next_id += 1
    config_id = next_id
    next_id += 1
    config_slot = slots[slot_index]

    reserved = set()
    config_blocks = allocate_blocks(image, layout,
                                     math.ceil(len(DEFAULT_NETWORK_CONFIG) / B),
                                     reserved)
    for index, block in enumerate(config_blocks):
        start = index * B
        image[block * B:(block + 1) * B] = DEFAULT_NETWORK_CONFIG[
            start:start + B].ljust(B, b"\0")
    config_offset = record_table_offset(layout, config_slot)
    write_record(image, config_offset, config_id, 1, 1,
                 DEFAULT_NETWORK_CONFIG, make_extents(config_blocks, 1), 0)

    network_payload += directory_entry(config_id, NETWORK_CONFIG_NAME)
    table_indices = {config_offset // B - layout["record_table_start"]}
    if network_id is None:
        raise RuntimeError("failed to allocate network directory")
    if network_slot is not None:
        network_offset = record_table_offset(layout, network_slot)
        network_blocks = allocate_blocks(image, layout,
                                         math.ceil(len(network_payload) / B),
                                         reserved)
        for index, block in enumerate(network_blocks):
            start = index * B
            image[block * B:(block + 1) * B] = network_payload[
                start:start + B].ljust(B, b"\0")
        write_record(image, network_offset, network_id, 2, 1,
                     network_payload, make_extents(network_blocks, 2), 0)
        table_indices.add(network_offset // B - layout["record_table_start"])
        core_payload += directory_entry(network_id, NETWORK_NAME)
    else:
        table_indices.add(replace_directory_payload(
            image, layout, network_id, network_payload, reserved))

    table_indices.add(replace_directory_payload(
        image, layout, core_id, core_payload, reserved))
    for table_index in table_indices:
        refresh_table_checksum(image, layout, table_index)
    refresh_record_bitmap_checksum(image, layout)
    refresh_metadata_checksums(image, layout)
    superblock = bytearray(image[:B])
    w64(superblock, 96, next_id)
    checksum(superblock, SUPER_CHECKSUM_OFFSET, 200)
    image[:B] = superblock
    return True


def _record_type(image, layout, record_id):
    offset = record_offset(image, layout["record_table_start"], record_id,
                           layout["record_count"])
    return u64(image, offset), offset


def _create_payload_record(image, layout, record_id, slot, record_type,
                           payload, extent_flags, reserved, owner_uid=0,
                           permissions=SYSTEM_PERMISSIONS):
    blocks = allocate_blocks(image, layout, math.ceil(len(payload) / B),
                             reserved)
    for index, block in enumerate(blocks):
        start = index * B
        image[block * B:(block + 1) * B] = payload[start:start + B].ljust(B, b"\0")
    extents = make_extents(blocks, extent_flags)
    list_count = math.ceil(max(0, len(extents) - 2) / EXTENTS_PER_LIST_BLOCK)
    list_blocks = allocate_blocks(image, layout, list_count, reserved)
    write_extent_lists(image, record_id, extents, list_blocks)
    write_record(image, record_table_offset(layout, slot), record_id,
                 record_type, 1, payload, extents,
                 list_blocks[0] if list_blocks else 0, owner_uid, permissions)
    return record_table_offset(layout, slot) // B - layout["record_table_start"]


def ensure_help_layout(image, layout):
    """Install /share/help records without disturbing guest-owned records."""
    root_offset = record_offset(image, layout["record_table_start"], 1,
                                layout["record_count"])
    root_record = bytes(image[root_offset:root_offset + RECORD_BYTES])
    if u64(root_record, 0) != 2:
        raise RuntimeError("root Record is not a directory")
    root_payload = record_payload(image, root_record)
    share_id = directory_find(root_payload, "share")
    if share_id is not None and _record_type(image, layout, share_id)[0] != 2:
        raise RuntimeError("root/share is not a directory")

    share_payload = b""
    if share_id is not None:
        share_offset = _record_type(image, layout, share_id)[1]
        share_payload = record_payload(
            image, bytes(image[share_offset:share_offset + RECORD_BYTES]))
    help_id = directory_find(share_payload, "help")
    if help_id is not None and _record_type(image, layout, help_id)[0] != 2:
        raise RuntimeError("share/help is not a directory")

    help_payload = b""
    if help_id is not None:
        help_offset = _record_type(image, layout, help_id)[1]
        help_payload = record_payload(
            image, bytes(image[help_offset:help_offset + RECORD_BYTES]))

    old_names = {"copy", "list", "move", "remove"}
    removed_ids = []
    for name in old_names:
        old_id = directory_find(help_payload, name)
        if old_id is not None:
            help_payload = directory_remove(help_payload, name)
            removed_ids.append(old_id)
    for old_id in removed_ids:
        release_record(image, layout, old_id)

    source_payloads = {
        name: open(os.path.join(HELP_SOURCE_DIR, name), "rb").read()
        for name in HELP_FILES
    }
    existing = {name: record_id for _, _, record_id, flags, name in
                directory_entries(help_payload) if flags == 1}
    for name in HELP_NAMES:
        record_id = existing.get(name.encode("utf-8"))
        if record_id is not None and _record_type(image, layout, record_id)[0] != 1:
            raise RuntimeError("share/help/%s is not a file" % name)

    missing_share = share_id is None
    missing_help = help_id is None
    missing_files = [name for name in HELP_NAMES
                     if name.encode("utf-8") not in existing]
    final_help_size = len(help_payload)
    for name in missing_files:
        final_help_size += len(directory_entry(0, name))
    source_payloads[HELP_INDEX_NAME] = build_help_index(
        source_payloads, final_help_size)
    missing_count = int(missing_share) + int(missing_help) + len(missing_files)
    used_ids = all_record_ids(image, layout)
    next_id = max(2, u64(image, 96), max(used_ids, default=1) + 1)
    reserved_slots = {record_id - 1 for record_id in
                      (set(SYSTEM_RECORDS) | {
                          NETWORK_RECORD_ID, NETWORK_CONFIG_RECORD_ID,
                          DEVELOPER_HOME_RECORD_ID, STATE_RECORD_ID,
                          ACCOUNTS_RECORD_ID, ACCOUNT_DATABASE_RECORD_ID,
                          SESSION_RECORD_ID, AUTOLOGIN_RECORD_ID,
                      }) if 0 < record_id <= layout["record_count"]}
    slots = allocate_record_slots(image, layout, missing_count,
                                   reserved_slots) if missing_count else []
    slot_index = 0
    reserved = set()
    table_indices = set()

    def allocate_id():
        nonlocal next_id
        while next_id in used_ids or next_id in SYSTEM_RECORDS:
            next_id += 1
        result = next_id
        used_ids.add(result)
        next_id += 1
        return result

    def create_directory():
        nonlocal slot_index
        record_id = allocate_id()
        slot = slots[slot_index]
        slot_index += 1
        table_indices.add(_create_payload_record(
            image, layout, record_id, slot, 2, b"", 2, reserved))
        return record_id

    changed = bool(removed_ids)
    if share_id is None:
        share_id = create_directory()
        root_payload += directory_entry(share_id, "share")
        changed = True
    if help_id is None:
        help_id = create_directory()
        share_payload += directory_entry(help_id, "help")
        changed = True

    for name in missing_files:
        record_id = allocate_id()
        slot = slots[slot_index]
        slot_index += 1
        table_indices.add(_create_payload_record(
            image, layout, record_id, slot, 1, source_payloads[name], 1,
            reserved))
        help_payload += directory_entry(record_id, name)
        existing[name.encode("utf-8")] = record_id
        changed = True

    share_offset = _record_type(image, layout, share_id)[1]
    old_share_payload = record_payload(
        image, bytes(image[share_offset:share_offset + RECORD_BYTES]))
    if share_payload != old_share_payload:
        table_indices.add(replace_directory_payload(
            image, layout, share_id, share_payload, reserved))
        changed = True
    help_offset = _record_type(image, layout, help_id)[1]
    old_help_payload = record_payload(
        image, bytes(image[help_offset:help_offset + RECORD_BYTES]))
    if help_payload != old_help_payload:
        table_indices.add(replace_directory_payload(
            image, layout, help_id, help_payload, reserved))
        changed = True
    if root_payload != record_payload(image, root_record):
        table_indices.add(replace_directory_payload(
            image, layout, 1, root_payload, reserved))
        changed = True

    for name in HELP_NAMES:
        record_id = existing.get(name.encode("utf-8"))
        offset = _record_type(image, layout, record_id)[1]
        record = bytes(image[offset:offset + RECORD_BYTES])
        if record_payload(image, record) != source_payloads[name]:
            table_indices.add(replace_file_payload(
                image, layout, record_id, source_payloads[name], reserved))
            changed = True
        security_index = rewrite_record_security(
            image, layout, record_id, VFS_UID_SYSTEM, SYSTEM_PERMISSIONS)
        if security_index is not None:
            table_indices.add(security_index)
            changed = True

    for record_id in (share_id, help_id):
        security_index = rewrite_record_security(
            image, layout, record_id, VFS_UID_SYSTEM, SYSTEM_PERMISSIONS)
        if security_index is not None:
            table_indices.add(security_index)
            changed = True

    for table_index in table_indices:
        refresh_table_checksum(image, layout, table_index)
    if not changed:
        return False
    refresh_record_bitmap_checksum(image, layout)
    refresh_metadata_checksums(image, layout)
    superblock = bytearray(image[:B])
    w64(superblock, 96, next_id)
    checksum(superblock, SUPER_CHECKSUM_OFFSET, 200)
    image[:B] = superblock
    return True


def migrate_command_names(image, layout):
    """Rename the old public file utilities in /bin without changing Records."""
    bin_offset = record_offset(image, layout["record_table_start"], 2,
                               layout["record_count"])
    bin_record = bytes(image[bin_offset:bin_offset + RECORD_BYTES])
    if u64(bin_record, 0) != 2:
        raise RuntimeError("bin Record is not a directory")
    payload = record_payload(image, bin_record)
    changed = False
    for old_name, new_name in (("copy", "cp"), ("list", "ls"),
                               ("move", "mv"), ("remove", "rm")):
        old_id = directory_find(payload, old_name)
        new_id = directory_find(payload, new_name)
        if old_id is None:
            continue
        if new_id is not None and new_id != old_id:
            raise RuntimeError("bin contains both %s and %s" %
                               (old_name, new_name))
        payload = directory_remove(payload, old_name)
        payload += directory_entry(old_id, new_name)
        changed = True
    if not changed:
        return False
    reserved = set()
    table_index = replace_directory_payload(image, layout, 2, payload, reserved)
    refresh_table_checksum(image, layout, table_index)
    refresh_record_bitmap_checksum(image, layout)
    refresh_metadata_checksums(image, layout)
    return True


def ensure_system_commands(image, layout, payloads):
    """Ensure every installed command has one file Record in /bin.

    Existing Records are found by their directory names.  Newly introduced
    commands receive ordinary free Record IDs so guest-created Records can
    never be overwritten by a later system update.
    """
    bin_offset = record_offset(image, layout["record_table_start"], 2,
                               layout["record_count"])
    bin_record = bytes(image[bin_offset:bin_offset + RECORD_BYTES])
    if u64(bin_record, 0) != 2:
        raise RuntimeError("bin Record is not a directory")
    bin_payload = record_payload(image, bin_record)
    command_records = {}
    for name in PAYLOAD_NAMES:
        record_id = directory_find(bin_payload, name)
        if record_id is not None:
            if name in command_records:
                raise RuntimeError("bin contains duplicate %s" % name)
            if _record_type(image, layout, record_id)[0] != 1:
                raise RuntimeError("bin/%s is not a file" % name)
            command_records[name] = record_id

    missing = [name for name in PAYLOAD_NAMES if name not in command_records]
    used_ids = all_record_ids(image, layout)
    next_id = max(2, u64(image, 96), max(used_ids, default=1) + 1)
    reserved_slots = {record_id - 1 for record_id in
                      (set(SYSTEM_RECORDS) | {
                          NETWORK_RECORD_ID, NETWORK_CONFIG_RECORD_ID,
                          DEVELOPER_HOME_RECORD_ID, STATE_RECORD_ID,
                          ACCOUNTS_RECORD_ID, ACCOUNT_DATABASE_RECORD_ID,
                          SESSION_RECORD_ID, AUTOLOGIN_RECORD_ID,
                      }) if 0 < record_id <= layout["record_count"]}
    slots = allocate_record_slots(image, layout, len(missing),
                                   reserved_slots) if missing else []
    reserved = set()
    table_indices = set()
    changed = False

    def allocate_id():
        nonlocal next_id
        while next_id in used_ids or next_id in SYSTEM_RECORDS:
            next_id += 1
        result = next_id
        used_ids.add(result)
        next_id += 1
        return result

    for index, name in enumerate(missing):
        record_id = allocate_id()
        slot = slots[index]
        table_indices.add(_create_payload_record(
            image, layout, record_id, slot, 1, payloads[name], 1, reserved))
        command_records[name] = record_id
        bin_payload += directory_entry(record_id, name)
        changed = True

    if bin_payload != record_payload(image, bin_record):
        table_indices.add(replace_directory_payload(
            image, layout, 2, bin_payload, reserved))
        changed = True

    for name, record_id in command_records.items():
        security_index = rewrite_record_security(
            image, layout, record_id, VFS_UID_SYSTEM, SYSTEM_PERMISSIONS)
        if security_index is not None:
            table_indices.add(security_index)
            changed = True
    for table_index in table_indices:
        refresh_table_checksum(image, layout, table_index)
    if changed:
        refresh_record_bitmap_checksum(image, layout)
        refresh_metadata_checksums(image, layout)
        superblock = bytearray(image[:B])
        w64(superblock, 96, next_id)
        checksum(superblock, SUPER_CHECKSUM_OFFSET, 200)
        image[:B] = superblock
    return command_records, changed


def ensure_autologin_policy(image, layout, username):
    """Install the explicit development-session autologin marker."""
    if not username:
        return False
    if not re.fullmatch(r"[a-z][a-z0-9_-]*", username):
        raise RuntimeError("invalid autologin username")

    root_offset = record_offset(image, layout["record_table_start"], 1,
                                layout["record_count"])
    root_payload = record_payload(image, bytes(image[root_offset:
                                                    root_offset + RECORD_BYTES]))
    core_id = directory_find(root_payload, "core")
    if core_id is None:
        raise RuntimeError("root directory has no core directory")
    core_offset = record_offset(image, layout["record_table_start"], core_id,
                                layout["record_count"])
    core_record = bytes(image[core_offset:core_offset + RECORD_BYTES])
    if u64(core_record, 0) != 2:
        raise RuntimeError("core Record is not a directory")
    core_payload = record_payload(image, core_record)
    session_id = directory_find(core_payload, "session")
    table_indices = set()
    used_ids = all_record_ids(image, layout)
    next_id = max(2, u64(image, 96), max(used_ids, default=1) + 1)
    reserved_slots = {record_id - 1 for record_id in RESERVED_SYSTEM_RECORD_IDS
                      if 0 < record_id <= layout["record_count"]}

    def allocate_id():
        nonlocal next_id
        while next_id in used_ids or next_id in RESERVED_SYSTEM_RECORD_IDS:
            next_id += 1
        result = next_id
        used_ids.add(result)
        next_id += 1
        return result

    if session_id is None:
        slots = allocate_record_slots(image, layout, 2, reserved_slots)
        session_id = allocate_id()
        marker_id = allocate_id()
        session_slot = slots[0]
        marker_slot = slots[1]
        marker_data = (username + "\n").encode("ascii")
        marker_blocks = allocate_blocks(image, layout, 1, set())
        image[marker_blocks[0] * B:(marker_blocks[0] + 1) * B] = \
            marker_data.ljust(B, b"\0")
        marker_offset = record_table_offset(layout, marker_slot)
        write_record(image, marker_offset, marker_id, 1, 1, marker_data,
                     make_extents(marker_blocks, 1), 0)
        session_data = directory_entry(marker_id, "autologin")
        session_blocks = allocate_blocks(image, layout, 1, set())
        image[session_blocks[0] * B:(session_blocks[0] + 1) * B] = \
            session_data.ljust(B, b"\0")
        session_offset = record_table_offset(layout, session_slot)
        write_record(image, session_offset, session_id, 2, 1, session_data,
                     make_extents(session_blocks, 2), 0)
        core_payload += directory_entry(session_id, "session")
        table_indices.update({record_slot_from_offset(
            marker_offset, layout["record_table_start"]) // RECORDS_PER_TABLE_BLOCK,
            record_slot_from_offset(
                session_offset, layout["record_table_start"]) // RECORDS_PER_TABLE_BLOCK})
        table_indices.add(replace_directory_payload(
            image, layout, core_id, core_payload, set()))
    else:
        session_offset = record_offset(image, layout["record_table_start"],
                                       session_id, layout["record_count"])
        session_record = bytes(image[session_offset:session_offset + RECORD_BYTES])
        if u64(session_record, 0) != 2:
            raise RuntimeError("core/session is not a directory")
        session_payload = record_payload(image, session_record)
        marker_id = directory_find(session_payload, "autologin")
        if marker_id is None:
            marker_id = allocate_id()
            marker_data = (username + "\n").encode("ascii")
            marker_blocks = allocate_blocks(image, layout, 1, set())
            image[marker_blocks[0] * B:(marker_blocks[0] + 1) * B] = \
                marker_data.ljust(B, b"\0")
            marker_offset = record_table_offset(
                layout, allocate_record_slots(image, layout, 1,
                                               reserved_slots)[0])
            write_record(image, marker_offset, marker_id, 1, 1, marker_data,
                         make_extents(marker_blocks, 1), 0)
            session_payload += directory_entry(marker_id, "autologin")
            table_indices.add(record_slot_from_offset(
                marker_offset, layout["record_table_start"]) // RECORDS_PER_TABLE_BLOCK)
            table_indices.add(replace_directory_payload(
                image, layout, session_id, session_payload, set()))
        else:
            marker_offset = record_offset(image, layout["record_table_start"],
                                          marker_id, layout["record_count"])
            marker_record = bytes(image[marker_offset:marker_offset + RECORD_BYTES])
            if u64(marker_record, 0) != 1:
                raise RuntimeError("session/autologin is not a file")
            marker_data = (username + "\n").encode("ascii")
            if record_payload(image, marker_record) != marker_data:
                table_indices.add(replace_file_payload(
                    image, layout, marker_id, marker_data, set()))

    for table_index in table_indices:
        refresh_table_checksum(image, layout, table_index)
    if not table_indices:
        return False
    refresh_record_bitmap_checksum(image, layout)
    refresh_metadata_checksums(image, layout)
    superblock = bytearray(image[:B])
    w64(superblock, 96, next_id)
    checksum(superblock, SUPER_CHECKSUM_OFFSET, 200)
    image[:B] = superblock
    return True


def rewrite_record_security(image, layout, record_id, owner_uid, permissions):
    offset = record_offset(image, layout["record_table_start"], record_id,
                           layout["record_count"])
    record = bytearray(image[offset:offset + RECORD_BYTES])
    old_flags = u64(record, 8)
    new_flags = record_flags(owner_uid, permissions,
                             bool(old_flags & RECORD_INLINE_DATA))
    if old_flags == new_flags:
        return None
    w64(record, 8, new_flags)
    checksum(record, RECORD_CHECKSUM_OFFSET, RECORD_BYTES)
    image[offset:offset + RECORD_BYTES] = record
    return record_slot_from_offset(offset, layout["record_table_start"]) // \
        RECORDS_PER_TABLE_BLOCK


def release_record(image, layout, record_id):
    offset = record_offset(image, layout["record_table_start"], record_id,
                           layout["record_count"])
    record = bytes(image[offset:offset + RECORD_BYTES])
    extents, list_blocks = record_extents(image, record)
    for _, physical, count, _ in extents:
        for block in range(physical, physical + count):
            set_allocated(image, layout, block, False)
    for block in list_blocks:
        set_allocated(image, layout, block, False)
    set_record_allocated(image, layout,
                         record_slot_from_offset(offset,
                                                 layout["record_table_start"]),
                         False)
    image[offset:offset + RECORD_BYTES] = b"\0" * RECORD_BYTES
    return record_slot_from_offset(offset, layout["record_table_start"]) // \
        RECORDS_PER_TABLE_BLOCK


def migrate_legacy_home(image, layout, root_payload, user_id, home_id):
    """Move legacy /home contents into /user and remove the old root entry."""
    if home_id is None:
        return root_payload, user_id, False, set()

    home_offset = record_offset(image, layout["record_table_start"], home_id,
                                layout["record_count"])
    home_record = bytes(image[home_offset:home_offset + RECORD_BYTES])
    if u64(home_record, 0) != 2:
        raise RuntimeError("legacy /home is not a directory")
    home_payload = record_payload(image, home_record)
    table_indices = set()

    if user_id is None:
        renamed = bytearray(root_payload)
        for entry_offset, entry_size, record_id, flags, name in directory_entries(root_payload):
            if flags == 1 and record_id == home_id and name == b"home":
                replacement = directory_entry(home_id, "user")
                if len(replacement) != entry_size:
                    raise RuntimeError("legacy /home entry cannot be renamed safely")
                renamed[entry_offset:entry_offset + entry_size] = replacement
                break
        else:
            raise RuntimeError("legacy /home entry disappeared during migration")
        return bytes(renamed), home_id, True, table_indices

    user_offset = record_offset(image, layout["record_table_start"], user_id,
                                layout["record_count"])
    user_record = bytes(image[user_offset:user_offset + RECORD_BYTES])
    if u64(user_record, 0) != 2:
        raise RuntimeError("/user is not a directory")
    user_payload = record_payload(image, user_record)
    existing = {name: record_id for _, _, record_id, flags, name
                in directory_entries(user_payload) if flags == 1}
    merged = bytearray(user_payload)
    for _, _, record_id, flags, name in directory_entries(home_payload):
        if flags != 1:
            continue
        if name in existing:
            if existing[name] != record_id:
                raise RuntimeError("legacy /home conflicts with /user")
            continue
        merged.extend(directory_entry(record_id, name.decode("utf-8")))
        existing[name] = record_id

    if bytes(merged) != user_payload:
        table_indices.add(replace_directory_payload(
            image, layout, user_id, bytes(merged), set()))

    root_without_home = bytearray()
    for _, _, record_id, flags, name in directory_entries(root_payload):
        if flags == 1 and record_id == home_id and name == b"home":
            continue
        root_without_home.extend(directory_entry(record_id, name.decode("utf-8")))
    table_indices.add(release_record(image, layout, home_id))
    return bytes(root_without_home), user_id, True, table_indices


def assign_subtree_security(image, layout, record_id, owner_uid, permissions,
                            visited=None):
    if visited is None:
        visited = set()
    if record_id in visited:
        raise RuntimeError("cycle in user directory tree")
    visited.add(record_id)
    offset = record_offset(image, layout["record_table_start"], record_id,
                           layout["record_count"])
    record = bytes(image[offset:offset + RECORD_BYTES])
    table_indices = set()
    changed = rewrite_record_security(image, layout, record_id, owner_uid,
                                      permissions)
    if changed is not None:
        table_indices.add(changed)
    if u64(record, 0) == 2:
        for _, _, child_id, flags, _ in directory_entries(record_payload(image, record)):
            if flags == 1:
                table_indices.update(assign_subtree_security(
                    image, layout, child_id, owner_uid, permissions, visited))
    visited.remove(record_id)
    return table_indices


def ensure_account_layout(image, layout):
    """Ensure /user/developer and /state/accounts exist.

    Older images may have the account directory below /core.  The directory
    Record is moved to /state without changing account Record IDs or payloads,
    then the legacy entry is removed so there is only one active database.
    """
    root_offset = record_offset(image, layout["record_table_start"], 1,
                                layout["record_count"])
    root_record = bytes(image[root_offset:root_offset + RECORD_BYTES])
    if u64(root_record, 0) != 2:
        raise RuntimeError("root Record is not a directory")
    root_payload = record_payload(image, root_record)

    def record_type(record_id):
        offset = record_offset(image, layout["record_table_start"], record_id,
                               layout["record_count"])
        return u64(image, offset), offset

    core_id = directory_find(root_payload, "core")
    if core_id is None:
        raise RuntimeError("root directory has no core directory")
    core_type, core_offset = record_type(core_id)
    if core_type != 2:
        raise RuntimeError("core Record is not a directory")
    core_record = bytes(image[core_offset:core_offset + RECORD_BYTES])
    core_payload = record_payload(image, core_record)

    state_id = directory_find(root_payload, "state")
    if state_id is not None and record_type(state_id)[0] != 2:
        raise RuntimeError("root/state is not a directory")
    state_payload = b""
    if state_id is not None:
        state_payload = record_payload(image, bytes(image[
            record_type(state_id)[1]:record_type(state_id)[1] + RECORD_BYTES]))

    user_id = directory_find(root_payload, "user")
    home_id = directory_find(root_payload, "home")
    if user_id is not None and record_type(user_id)[0] != 2:
        raise RuntimeError("root/user is not a directory")
    if home_id is not None and record_type(home_id)[0] != 2:
        raise RuntimeError("root/home is not a directory")

    core_accounts_id = directory_find(core_payload, "accounts")
    state_accounts_id = directory_find(state_payload, "accounts")
    if core_accounts_id is not None and state_accounts_id is not None:
        raise RuntimeError("both core/accounts and state/accounts exist")
    accounts_id = state_accounts_id if state_accounts_id is not None else \
        core_accounts_id
    if accounts_id is not None and record_type(accounts_id)[0] != 2:
        raise RuntimeError("account directory is not a directory")

    changed = False
    table_indices = set()
    if home_id is not None:
        root_payload, user_id, migrated, migration_indices = migrate_legacy_home(
            image, layout, root_payload, user_id, home_id)
        if migrated:
            changed = True
            table_indices.update(migration_indices)

    user_payload = b""
    if user_id is not None:
        user_payload = record_payload(image, bytes(image[
            record_type(user_id)[1]:record_type(user_id)[1] + RECORD_BYTES]))
    developer_id = directory_find(user_payload, "developer")
    if developer_id is not None and record_type(developer_id)[0] != 2:
        raise RuntimeError("user/developer is not a directory")

    accounts_payload = b""
    if accounts_id is not None:
        accounts_payload = record_payload(image, bytes(image[
            record_type(accounts_id)[1]:record_type(accounts_id)[1] + RECORD_BYTES]))
    database_id = directory_find(accounts_payload, ACCOUNT_DATABASE_NAME)
    legacy_database_id = directory_find(accounts_payload,
                                        LEGACY_ACCOUNT_DATABASE_NAME)
    if database_id is not None and legacy_database_id is not None:
        raise RuntimeError("account directory contains both databases")
    if database_id is None:
        database_id = legacy_database_id
    if database_id is not None and record_type(database_id)[0] != 1:
        raise RuntimeError("account database is not a file")

    missing = (user_id is None, developer_id is None, state_id is None,
               accounts_id is None, database_id is None)
    used_ids = all_record_ids(image, layout)
    next_id = max(2, u64(image, 96), max(used_ids, default=1) + 1)
    reserved_slots = {record_id - 1 for record_id in
                      RESERVED_SYSTEM_RECORD_IDS
                      if 0 < record_id <= layout["record_count"]}
    slots = allocate_record_slots(image, layout, sum(missing), reserved_slots) \
        if any(missing) else []
    slot_index = 0
    reserved = set()

    def allocate_id():
        nonlocal next_id
        while next_id in used_ids or next_id in RESERVED_SYSTEM_RECORD_IDS:
            next_id += 1
        result = next_id
        used_ids.add(result)
        next_id += 1
        return result

    def create_empty_directory(owner_uid=VFS_UID_SYSTEM,
                               permissions=SYSTEM_PERMISSIONS):
        nonlocal slot_index
        record_id = allocate_id()
        offset = record_table_offset(layout, slots[slot_index])
        slot_index += 1
        write_record(image, offset, record_id, 2, 1, b"", [], 0,
                     owner_uid, permissions)
        table_indices.add(record_slot_from_offset(
            offset, layout["record_table_start"]) // RECORDS_PER_TABLE_BLOCK)
        return record_id

    def create_database():
        nonlocal slot_index
        record_id = allocate_id()
        offset = record_table_offset(layout, slots[slot_index])
        slot_index += 1
        blocks = allocate_blocks(
            image, layout, math.ceil(len(DEFAULT_ACCOUNT_DATABASE) / B),
            reserved)
        for index, block in enumerate(blocks):
            start = index * B
            image[block * B:(block + 1) * B] = DEFAULT_ACCOUNT_DATABASE[
                start:start + B].ljust(B, b"\0")
        write_record(image, offset, record_id, 1, 1,
                     DEFAULT_ACCOUNT_DATABASE, make_extents(blocks, 1), 0,
                     VFS_UID_SYSTEM, SYSTEM_PERMISSIONS)
        table_indices.add(record_slot_from_offset(
            offset, layout["record_table_start"]) // RECORDS_PER_TABLE_BLOCK)
        return record_id

    if user_id is None:
        user_id = create_empty_directory()
        root_payload += directory_entry(user_id, "user")
        changed = True
    if developer_id is None:
        developer_id = create_empty_directory(DEVELOPER_UID, USER_PERMISSIONS)
        user_payload += directory_entry(developer_id, "developer")
        changed = True
    if state_id is None:
        state_id = create_empty_directory()
        root_payload += directory_entry(state_id, "state")
        changed = True
    if accounts_id is None:
        accounts_id = create_empty_directory()
        state_payload += directory_entry(accounts_id, "accounts")
        changed = True
    elif core_accounts_id is not None:
        core_payload = directory_remove(core_payload, "accounts")
        state_payload += directory_entry(accounts_id, "accounts")
        changed = True
    if database_id is None:
        database_id = create_database()
        accounts_payload += directory_entry(database_id, ACCOUNT_DATABASE_NAME)
        changed = True
    elif legacy_database_id is not None:
        accounts_payload = directory_rename(
            accounts_payload, LEGACY_ACCOUNT_DATABASE_NAME,
            ACCOUNT_DATABASE_NAME)
        changed = True

    if developer_id is not None:
        if directory_find(user_payload, "developer") != developer_id:
            raise RuntimeError("user/developer entry was not installed")
        old_user_payload = record_payload(image, bytes(image[
            record_type(user_id)[1]:record_type(user_id)[1] + RECORD_BYTES]))
        if user_payload != old_user_payload:
            table_indices.add(replace_directory_payload(
                image, layout, user_id, user_payload, reserved))
    if accounts_id is not None:
        old_accounts_payload = record_payload(image, bytes(image[
            record_type(accounts_id)[1]:record_type(accounts_id)[1] + RECORD_BYTES]))
        if accounts_payload != old_accounts_payload:
            table_indices.add(replace_directory_payload(
                image, layout, accounts_id, accounts_payload, reserved))
    old_state_payload = b""
    if state_id is not None:
        old_state_payload = record_payload(image, bytes(image[
            record_type(state_id)[1]:record_type(state_id)[1] + RECORD_BYTES]))
        if state_payload != old_state_payload:
            table_indices.add(replace_directory_payload(
                image, layout, state_id, state_payload, reserved))
    if core_payload != record_payload(image, core_record):
        table_indices.add(replace_directory_payload(
            image, layout, core_id, core_payload, reserved))
    old_root_payload = record_payload(image, root_record)
    if root_payload != old_root_payload:
        table_indices.add(replace_directory_payload(
            image, layout, 1, root_payload, reserved))

    if database_id is not None:
        database_offset = record_type(database_id)[1]
        database_record = bytes(image[database_offset:database_offset + RECORD_BYTES])
        database_payload = record_payload(image, database_record)
        migrated_database, database_changed = migrate_account_database(
            database_payload)
        if database_changed:
            table_indices.add(replace_file_payload(
                image, layout, database_id, migrated_database, reserved))
            changed = True

    if developer_id is not None:
        security_indices = assign_subtree_security(
            image, layout, developer_id, DEVELOPER_UID, USER_PERMISSIONS)
        table_indices.update(security_indices)
        changed = changed or bool(security_indices)
    if table_indices:
        changed = True

    for record_id in (state_id, accounts_id, database_id):
        security_index = rewrite_record_security(
            image, layout, record_id, VFS_UID_SYSTEM, SYSTEM_PERMISSIONS)
        if security_index is not None:
            table_indices.add(security_index)
            changed = True

    for table_index in table_indices:
        refresh_table_checksum(image, layout, table_index)
    refresh_record_bitmap_checksum(image, layout)
    refresh_metadata_checksums(image, layout)
    superblock = bytearray(image[:B])
    w64(superblock, 96, next_id)
    checksum(superblock, SUPER_CHECKSUM_OFFSET, 200)
    image[:B] = superblock
    return changed


def refresh_metadata_checksums(image, layout):
    for index in range(layout["allocation_bitmap_blocks"]):
        offset = (layout["allocation_bitmap_start"] + index) * B
        block = bytearray(image[offset:offset + B])
        checksum(block, METADATA_CHECKSUM_OFFSET, B)
        image[offset:offset + B] = block


def upgrade_security_metadata(image, layout):
    """Upgrade legacy v1.0 records without interpreting old flag bits."""
    table_indices = set()
    for slot in range(layout["record_count"]):
        if not record_slot_allocated(image, layout, slot):
            continue
        offset = record_table_offset(layout, slot)
        record = bytearray(image[offset:offset + RECORD_BYTES])
        record_type = u64(record, 0)
        if record_type not in (1, 2):
            continue
        old_flags = u64(record, 8)
        w64(record, 8, record_flags(
            VFS_UID_SYSTEM, SYSTEM_PERMISSIONS,
            bool(old_flags & RECORD_INLINE_DATA)))
        checksum(record, RECORD_CHECKSUM_OFFSET, RECORD_BYTES)
        image[offset:offset + RECORD_BYTES] = record
        table_indices.add(slot // RECORDS_PER_TABLE_BLOCK)

    for table_index in table_indices:
        refresh_table_checksum(image, layout, table_index)
    superblock = bytearray(image[:B])
    w64(superblock, 16, MGFS_FORMAT_MINOR)
    checksum(superblock, SUPER_CHECKSUM_OFFSET, 200)
    image[:B] = superblock
    return True


def update(image_path, payload_paths, autologin=None):
    image = bytearray(open(image_path, "rb").read())
    if image[:8] != MAGIC:
        raise RuntimeError("not an MGFS v1 image")
    format_minor = u64(image, 16)
    if format_minor not in (0, MGFS_FORMAT_MINOR):
        raise RuntimeError("unsupported MGFS format minor version")

    total_blocks = u64(image, 40)
    if total_blocks * B != len(image):
        raise RuntimeError("unsupported MGFS geometry")
    layout = {
        "allocation_bitmap_start": u64(image, 104),
        "allocation_bitmap_blocks": u64(image, 112),
        "record_bitmap_start": u64(image, 120),
        "record_bitmap_blocks": u64(image, 128),
        "record_table_start": u64(image, 136),
        "record_table_blocks": u64(image, 144),
        "record_count": u64(image, 152),
        "data_start": u64(image, 160),
        "data_blocks": u64(image, 168),
    }
    if len(payload_paths) != len(PAYLOAD_NAMES):
        raise RuntimeError("wrong number of system payloads")
    payloads = {name: open(path, "rb").read()
                for name, path in zip(PAYLOAD_NAMES, payload_paths)}
    persistent_changed = False
    if format_minor == 0:
        persistent_changed = upgrade_security_metadata(image, layout)
    persistent_changed = ensure_network_config(image, layout) or persistent_changed
    if ensure_account_layout(image, layout):
        persistent_changed = True
    if migrate_command_names(image, layout):
        persistent_changed = True
    if ensure_help_layout(image, layout):
        persistent_changed = True
    command_records, commands_changed = ensure_system_commands(
        image, layout, payloads)
    if commands_changed:
        persistent_changed = True
    system_records = {command_records[name]: name for name in PAYLOAD_NAMES}
    bin_data = b"".join(directory_entry(command_records[name], name)
                         for name in PAYLOAD_NAMES)
    active_records = [2] + list(system_records)
    if ensure_autologin_policy(image, layout, autologin):
        persistent_changed = True
    offsets = {record_id: record_offset(image, layout["record_table_start"],
                                         record_id, layout["record_count"])
               for record_id in active_records}
    if not all(record_extent_layout_valid(
            image, layout, bytes(image[offsets[record_id]:
                                       offsets[record_id] + RECORD_BYTES]))
               for record_id in active_records):
        persistent_changed = True

    if (record_payload(image, bytes(image[offsets[2]:offsets[2] + RECORD_BYTES])) ==
            bin_data and
            all(record_payload(image, bytes(image[offsets[record_id]:
                                                  offsets[record_id] + RECORD_BYTES])) ==
                payloads[system_records[record_id]]
                for record_id in system_records) and
            not persistent_changed):
        return False

    reserved = set()
    old_state = {}
    for record_id, offset in offsets.items():
        record = bytes(image[offset:offset + RECORD_BYTES])
        extents, list_blocks = record_extents(image, record)
        data_blocks = []
        for logical, physical, count, flags in extents:
            if count == 0 or flags not in (1, 2) or \
                    physical < layout["data_start"] or \
                    physical + count > layout["data_start"] + layout["data_blocks"]:
                raise RuntimeError("invalid system extent")
            data_blocks.extend(range(physical, physical + count))
        old_state[record_id] = (u64(record, 24), extents, list_blocks, data_blocks)
        for block in data_blocks + list_blocks:
            set_allocated(image, layout, block, False)

    for record_id, offset in offsets.items():
        payload = bin_data if record_id == 2 else payloads[system_records[record_id]]
        needed = math.ceil(len(payload) / B)
        data_blocks = allocate_blocks(image, layout, needed, reserved)
        extents = make_extents(data_blocks, 2 if record_id == 2 else 1)
        list_count = math.ceil(max(0, len(extents) - 2) / EXTENTS_PER_LIST_BLOCK)
        list_blocks = allocate_blocks(image, layout, list_count, reserved)
        for index, block in enumerate(data_blocks):
            start = index * B
            image[block * B:(block + 1) * B] = payload[start:start + B].ljust(B, b"\0")
        write_extent_lists(image, record_id, extents, list_blocks)
        generation = old_state[record_id][0] + 1
        write_record(image, offset, record_id, 2 if record_id == 2 else 1,
                     generation, payload, extents,
                     list_blocks[0] if list_blocks else 0)

    table_indices = {
        (offset - layout["record_table_start"] * B) // B
        for offset in offsets.values()
    }
    for index in table_indices:
        offset = (layout["record_table_start"] + index) * B
        block = bytearray(image[offset:offset + B])
        checksum(block, METADATA_CHECKSUM_OFFSET, B)
        image[offset:offset + B] = block

    record_bitmap = bytearray(image[layout["record_bitmap_start"] * B:
                                    (layout["record_bitmap_start"] + 1) * B])
    for record_id in active_records:
        bit = record_id - 1
        record_bitmap[24 + bit // 8] |= 1 << (bit % 8)
    checksum(record_bitmap, METADATA_CHECKSUM_OFFSET, B)
    image[layout["record_bitmap_start"] * B:
          (layout["record_bitmap_start"] + 1) * B] = record_bitmap

    refresh_metadata_checksums(image, layout)
    superblock = bytearray(image[:B])
    w64(superblock, 64, 1)
    checksum(superblock, SUPER_CHECKSUM_OFFSET, 200)
    image[:B] = superblock

    temporary = image_path + ".update-tmp"
    with open(temporary, "wb") as output:
        output.write(image)
    os.replace(temporary, image_path)
    return True


def main():
    if len(sys.argv) not in (27, 28):
        raise SystemExit("usage: update_mgfs.py <image> "
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
            raise SystemExit("update_mgfs: invalid image policy")
        autologin = sys.argv[27][len("--autologin="):]
    try:
        update(sys.argv[1], sys.argv[2:27], autologin)
    except (OSError, RuntimeError, ValueError) as error:
        raise SystemExit("update_mgfs: %s" % error)


if __name__ == "__main__":
    main()
