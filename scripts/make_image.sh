#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -e

BOOT_IMAGE=build/Mangrove/Boot.img
ROOT_IMAGE=build/Mangrove/Mangrove.img
MKMGFS=build/mkmgfs
PITH=build/Mangrove/pith.elf
SPROUT=build/Sprout/sprout.elf
SPROUT_CMD=build/SproutCmd/sprout.elf
SESSIOND=build/Sessiond/sessiond.elf
LOGIND=build/Logind/logind.elf
LOGD=build/Logd/logd.elf
NETWORKD=build/Networkd/networkd.elf
DEVICED=build/Deviced/deviced.elf
VOLUMED=build/Volumed/volumed.elf
MOUNT=build/Mount/mount.elf
UNMOUNT=build/Unmount/unmount.elf
EJECT=build/Eject/eject.elf
DISKUTIL=build/Diskutil/diskutil.elf
LSPCI=build/Lspci/lspci.elf
LSUSB=build/Lsusb/lsusb.elf
LSDISK=build/Lsdsk/lsdsk.elf
TASK=build/Task/task.elf
MEM=build/Mem/mem.elf
TIME=build/Time/time.elf
TMON=build/Tmon/tmon.elf
LOGV=build/Logv/logv.elf
SHOOT=build/Shoot/shoot.elf
CLEAR=build/Clear/clear.elf
CP=build/Cp/cp.elf
LS=build/Ls/ls.elf
LOCATE=build/Locate/locate.elf
MV=build/Mv/mv.elf
PLANT=build/Plant/plant.elf
READ=build/Read/read.elf
RM=build/Rm/rm.elf
MKDIR=build/Mkdir/mkdir.elf
RMDIR=build/Rmdir/rmdir.elf
SAY=build/Say/say.elf
UPTIME=build/Uptime/uptime.elf
DATE=build/Date/date.elf
PING=build/Ping/ping.elf
RESOLVE=build/Resolve/resolve.elf
FETCH=build/Fetch/fetch.elf
NETINFO=build/Netinfo/netinfo.elf
NETCFG=build/Netcfg/netcfg.elf
POWER=build/Power/power.elf
IDENTITY=build/Identity/identity.elf
USER_CMD=build/User/user.elf
SHUTDOWN=build/Shutdown/shutdown.elf
REBOOT=build/Reboot/reboot.elf
VERSION=build/Version/version.elf
WHERE=build/Where/where.elf
PCI_IDS=share/hardware/pci.ids
USB_IDS=share/hardware/usb.ids
HARDWARE_README=share/hardware/README.txt
FRESH=0
AUTOLOGIN=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --fresh)
            FRESH=1
            ;;
        --root)
            shift
            if [ "$#" -eq 0 ]; then
                echo "Usage: $0 [--fresh] [--root image]" >&2
                exit 2
            fi
            ROOT_IMAGE=$1
            ;;
        --autologin)
            shift
            if [ "$#" -eq 0 ]; then
                echo "Usage: $0 [--fresh] [--root image] [--autologin user]" >&2
                exit 2
            fi
            AUTOLOGIN=$1
            ;;
        *)
            echo "Usage: $0 [--fresh] [--root image] [--autologin user]" >&2
            exit 2
            ;;
    esac
    shift
done

mkdir -p build/Mangrove
mkdir -p "$(dirname "$ROOT_IMAGE")"

# Keep the ordered payload list in one place for both fresh population and
# incremental updates.  The Python tools validate its length against their
# shared canonical manifest.
run_mgfs_tool() {
    tool="$1"
    shift
    python3 "tools/${tool}.py" "$ROOT_IMAGE" \
        "$PITH" "$SPROUT" "$SESSIOND" "$LOGIND" "$LOGD" "$NETWORKD" \
        "$DEVICED" "$VOLUMED" "$LSPCI" "$LSUSB" "$LSDISK" "$TASK" "$MEM" "$TIME" "$TMON" \
        "$LOGV" "$MOUNT" "$UNMOUNT" "$EJECT" "$DISKUTIL" \
        "$SHOOT" "$CLEAR" "$CP" "$SAY" "$UPTIME" "$LS" "$LOCATE" \
        "$MV" "$PLANT" "$READ" "$RM" "$VERSION" "$WHERE" "$PING" \
        "$RESOLVE" "$FETCH" "$NETINFO" "$NETCFG" "$SHUTDOWN" \
        "$REBOOT" "$POWER" \
        "$IDENTITY" "$USER_CMD" "$MKDIR" "$RMDIR" "$SPROUT_CMD" "$DATE" \
        "$PCI_IDS" "$USB_IDS" "$HARDWARE_README" \
        "$@"
}

rm -f "$BOOT_IMAGE"

# A zero-count seek creates the same sparse 64 MiB image with GNU or BSD dd.
dd if=/dev/zero of="$BOOT_IMAGE" bs=1 count=0 seek=67108864 2>/dev/null
mkfs.fat -F32 "$BOOT_IMAGE"

mmd -i "$BOOT_IMAGE" ::/EFI
mmd -i "$BOOT_IMAGE" ::/EFI/BOOT

mcopy -i "$BOOT_IMAGE" build/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/

if [ "$FRESH" -eq 1 ]; then
    echo "[IMAGE] Resetting MGFS image: $ROOT_IMAGE"
    rm -f "$ROOT_IMAGE"
fi

if [ -f "$ROOT_IMAGE" ] && ! head -c 8 "$ROOT_IMAGE" | grep -a -q 'MGFSv1'; then
    echo "Discarding obsolete non-MGFS root image $ROOT_IMAGE"
    rm -f "$ROOT_IMAGE"
fi

if [ ! -f "$ROOT_IMAGE" ]; then
    echo "Creating fresh MGFS root image $ROOT_IMAGE..."
    "$MKMGFS" \
        --blocks 16384 \
        --uuid 00000000-0000-0000-0000-000000000001 \
        --format-time-ns 0 \
        "$ROOT_IMAGE"
    if [ -n "$AUTOLOGIN" ]; then
        run_mgfs_tool populate_mgfs "--autologin=$AUTOLOGIN"
    else
        run_mgfs_tool populate_mgfs
    fi
else
    if [ -n "$AUTOLOGIN" ]; then
        run_mgfs_tool update_mgfs "--autologin=$AUTOLOGIN"
    else
    run_mgfs_tool update_mgfs
    fi
fi
