#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real native/formatting/private-owner paths; controlled registry and I/O.

--baseline replays the two unsafe behaviors from the committed source in a
temporary directory. The default executes fixed-path sanitizer regressions and
independent disposable-image checks when their tools are available.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[2] / "shared"))
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[3]
baseline = "--baseline" in sys.argv
baseline_ref = "3c16cf3cac8ddca5a32caba691be704f8cc8b0b8"
compiler = next((arg for arg in sys.argv[1:] if not arg.startswith("--")), "gcc")


def read(path):
    if baseline:
        return subprocess.check_output(["git", "show", baseline_ref + ":" + path], cwd=root).decode()
    return (root / path).read_text()


def function(source, name):
    for match in re.finditer(r"^(?:static )?[^\n;{}]*\b" + name + r"\s*\(", source, re.M):
        brace = source.find("{", match.end())
        if ";" in source[match.end():brace]:
            continue
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise AssertionError(name)


vfs = read("kernel/src/vfs.c")
native = read("kernel/src/syscall.c")
fat = read("kernel/src/storage/fat32.c")
with tempfile.TemporaryDirectory(prefix="mangrove-format-") as temporary:
    directory = Path(temporary)
    registry = vfs[vfs.index("typedef struct vfs_recovery_pin {"):vfs.index("static vfs_mount_t *vfs_find_mount_child")]
    registry += "\nstatic void *thread_current(void) { return (void *)1; }\nstatic vfs_mount_t mount_table[VFS_MAX_MOUNTS];\nstatic vfs_super_t dead_super;\nstatic spinlock_t vfs_metadata_lock;\nstatic mutex_t vfs_mount_lock;\n"
    if not baseline:
        start = vfs.index("struct vfs_location {")
        registry += vfs[start:vfs.index("\n};", start) + 4] + "\n"
    names = ("vfs_operation_guard_owned", "vfs_device_range", "vfs_device_already_mounted", "vfs_retarget_nodes_locked",
             "vfs_super_is_live_locked", "vfs_super_mark_dispose_locked", *(() if baseline else ("location_prune_locked", "location_free_list", "vfs_location_release",)), "vfs_super_destroy",
             "vfs_super_release", "vfs_recovery_private_begin", "vfs_recovery_private_release",
             "vfs_recovery_pin_mark_unresolved", "vfs_recovery_pin_finish_handoff", "vfs_format_preflight")
    physical = function((root / "kernel/src/block.c").read_text(), "block_device_physical_range")
    (directory / "format_vfs.inc").write_text(registry + physical + "\n".join(function(vfs, name) for name in names))
    probe = function(vfs, "vfs_probe_filesystem")
    if "const char *vfs_probe_filesystem_for_id(" in vfs:
        probe = function(vfs, "vfs_probe_filesystem_for_id") + probe
    (directory / "format_probe.inc").write_text(probe)
    first = fat.index("#pragma pack(push, 1)")
    last = fat.index("} fat32_bpb_t;", first) + len("} fat32_bpb_t;")
    fat_probe = fat[first:last] + "\n#pragma pack(pop)\n"
    fat_probe += "\n".join(function(fat, name) for name in ("fat32_probe_for_id", "fat32_probe"))
    (directory / "format_fat_probe.inc").write_text(fat_probe)
    start = native.index("        case SYSCALL_STORAGE_FORMAT: {")
    end = native.index("        case SYSCALL_STORAGE_LABEL:", start)
    syscall = r'''
typedef struct { bool storage_management_session; u32 storage_authorized_operation; u64 storage_authorized_thread_id; } format_process_t;
#define process_t format_process_t
static format_process_t requester;
static format_process_t *process_current(void) { return &requester; }
static bool syscall_is_diskutil(format_process_t *p) { return p == &requester; }
static u64 process_current_thread_id(void) { return 1U; }
static bool process_storage_authorization_consume(format_process_t *p,u64 id,u32 op) {
    if (!p || !p->storage_management_session || p->storage_authorized_thread_id != id ||
        p->storage_authorized_operation != op) return false;
    p->storage_authorized_operation=0U; p->storage_authorized_thread_id=0U; return true;
}
static bool syscall_user_buffer_valid(const void *p, usize n) { return p && n; }
block_device_t *block_get_device_by_public_id(u64 id) { return id == (BLOCK_DEVICE_ID_BASE | (device.id + 1U)) ? &device : NULL; }
static bool syscall_storage_device_matches(block_device_t *d,u64 parent) { return d == &device && !parent; }
static bool syscall_storage_system_managed(block_device_t *d) { (void)d; if(native_swap) { device.id++; native_swap=false; } return system_managed; }
bool block_device_set_automount_suppressed(u64 id,bool suppressed) { assert(suppressed && id == device.id); return true; }
#define SYSCALL_STORAGE_FORMAT 73U
typedef struct { u64 rax,rdi; } format_frame_t;
static void syscall_fail(format_frame_t *f,i64 result) { f->rax=(u64)result; }
'''
    syscall += function(native, "syscall_vfs_error")
    syscall += "static void native_dispatch(format_frame_t *frame) { switch(frame->rax) {\n" + native[start:end] + "default: assert(0); } }\n#undef process_t\n"
    syscall += r'''
static i64 native_format(u32 selection) {
    mg_storage_format_request_t req={.instance_id=BLOCK_DEVICE_ID_BASE | (device.id+1U),.filesystem=selection};
    requester.storage_management_session=true; requester.storage_authorized_operation=MG_STORAGE_OP_FORMAT;
    requester.storage_authorized_thread_id=1U;
    format_frame_t f={.rax=SYSCALL_STORAGE_FORMAT,.rdi=(u64)(usize)&req};
    native_dispatch(&f); assert(!requester.storage_authorized_operation &&
        !requester.storage_authorized_thread_id); return (i64)f.rax;
}
static i64 native_format_unauthorized(void) {
    mg_storage_format_request_t req={.instance_id=BLOCK_DEVICE_ID_BASE | (device.id+1U),.filesystem=MG_STORAGE_FILESYSTEM_EXFAT};
    requester.storage_management_session=false; requester.storage_authorized_operation=0;
    requester.storage_authorized_thread_id=0;
    format_frame_t f={.rax=SYSCALL_STORAGE_FORMAT,.rdi=(u64)(usize)&req};
    native_dispatch(&f); return (i64)f.rax;
}
'''
    (directory / "format_native.inc").write_text(syscall)
    fixture = (root / "tests/filesystems/exfat/storage_format_test.c").read_text()
    for name in ("exfat", "format"):
        source_path = root / "kernel/src/storage" / (name + ".c")
        if baseline:
            path = directory / (name + ".c")
            path.write_text(read("kernel/src/storage/" + name + ".c"))
            source_path = path
        fixture = fixture.replace(
            '"../../../kernel/src/storage/' + name + '.c"',
            '"' + str(source_path) + '"')
    fixture = fixture.replace(
        '"../../../libc/src/time_convert.c"',
        '"' + str(root / "libc/src/time_convert.c") + '"')
    # exFAT's table is a data dependency for the isolated baseline too.
    (directory / "exfat_upcase.inc").write_bytes((root / "kernel/src/storage/exfat_upcase.inc").read_bytes())
    source = directory / "fixture.c"
    source.write_text(fixture)
    binary = directory / "format-test"
    subprocess.run([compiler, "-O1", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-type-limits", "-ffunction-sections", "-fdata-sections",
                    "-fsanitize=address,undefined", "-pthread", *dead_strip_flags(),
                    *( ["-DFORMAT_BASELINE"] if baseline else []),
                    "-I" + str(directory), "-I" + str(root / "kernel/include"), "-I" + str(root / "include"),
                    "-idirafter", str(root / "libc/include"), str(source),
                    str(root / "libc/src/utf8.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
    if not baseline:
        search = os.environ.get("PATH", "") + os.pathsep + "/usr/sbin"
        for name, checker in (("fat32", "fsck.fat"), ("exfat", "fsck.exfat")):
            tool = shutil.which(checker, path=search)
            if not tool:
                print("UNAVAILABLE: independent " + name + " image check requires " + checker)
                continue
            image = directory / (name + ".img")
            with image.open("wb") as stream:
                stream.truncate(70000 * 512)
            subprocess.run([str(binary), "--image", name, str(image)], check=True)
            subprocess.run([tool, "-n", str(image)], check=True)
