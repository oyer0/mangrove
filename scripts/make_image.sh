#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
BUILD_DIR=${BUILD_DIR:-build}
ROOT_IMAGE=
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
        --build-dir)
            shift
            if [ "$#" -eq 0 ]; then
                echo "Usage: $0 [--fresh] [--build-dir directory] [--root image] [--autologin user]" >&2
                exit 2
            fi
            BUILD_DIR=$1
            ;;
        --autologin)
            shift
            if [ "$#" -eq 0 ]; then
                echo "Usage: $0 [--fresh] [--build-dir directory] [--root image] [--autologin user]" >&2
                exit 2
            fi
            AUTOLOGIN=$1
            ;;
        *)
            echo "Usage: $0 [--fresh] [--build-dir directory] [--root image] [--autologin user]" >&2
            exit 2
            ;;
    esac
    shift
done

case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$ROOT/$BUILD_DIR" ;;
esac
if [ -z "$ROOT_IMAGE" ]; then
    ROOT_IMAGE="$BUILD_DIR/Mangrove/Mangrove.img"
fi

BOOT_IMAGE="$BUILD_DIR/Mangrove/Boot.img"
MKMGFS="$BUILD_DIR/mkmgfs"
PITH="$BUILD_DIR/Mangrove/pith.elf"
SPROUT="$BUILD_DIR/sprout/sprout.elf"
SPROUT_CMD="$BUILD_DIR/sproutcmd/sprout.elf"
SESSIOND="$BUILD_DIR/sessiond/sessiond.elf"
LOGIND="$BUILD_DIR/logind/logind.elf"
LOGD="$BUILD_DIR/logd/logd.elf"
NETWORKD="$BUILD_DIR/networkd/networkd.elf"
DEVICED="$BUILD_DIR/deviced/deviced.elf"
VOLUMED="$BUILD_DIR/volumed/volumed.elf"
CLIPD="$BUILD_DIR/clipd/clipd.elf"
MOUNT="$BUILD_DIR/mount/mount.elf"
UNMOUNT="$BUILD_DIR/unmount/unmount.elf"
EJECT="$BUILD_DIR/eject/eject.elf"
DISKUTIL="$BUILD_DIR/diskutil/diskutil.elf"
LSPCI="$BUILD_DIR/lspci/lspci.elf"
LSUSB="$BUILD_DIR/lsusb/lsusb.elf"
LSDISK="$BUILD_DIR/lsdsk/lsdsk.elf"
CREW="$BUILD_DIR/crew/crew.elf"
MEM="$BUILD_DIR/mem/mem.elf"
TIME="$BUILD_DIR/time/time.elf"
TMON="$BUILD_DIR/tmon/tmon.elf"
LOGV="$BUILD_DIR/logv/logv.elf"
SHOOT="$BUILD_DIR/shoot/shoot.elf"
CLEAR="$BUILD_DIR/clear/clear.elf"
COPY="$BUILD_DIR/copy/copy.elf"
LIST="$BUILD_DIR/list/list.elf"
LOCATE="$BUILD_DIR/locate/locate.elf"
MOVE="$BUILD_DIR/move/move.elf"
PLANT="$BUILD_DIR/plant/plant.elf"
TYPE="$BUILD_DIR/type/type.elf"
WAIT="$BUILD_DIR/wait/wait.elf"
DEL="$BUILD_DIR/del/del.elf"
MKDIR="$BUILD_DIR/mkdir/mkdir.elf"
SAY="$BUILD_DIR/say/say.elf"
UPTIME="$BUILD_DIR/uptime/uptime.elf"
DATE="$BUILD_DIR/date/date.elf"
ABOUT="$BUILD_DIR/about/about.elf"
REED="$BUILD_DIR/reed/reed.elf"
VIEW="$BUILD_DIR/view/view.elf"
INFO="$BUILD_DIR/info/info.elf"
PING="$BUILD_DIR/ping/ping.elf"
RESOLVE="$BUILD_DIR/resolve/resolve.elf"
FETCH="$BUILD_DIR/fetch/fetch.elf"
NETINFO="$BUILD_DIR/netinfo/netinfo.elf"
NETCFG="$BUILD_DIR/netcfg/netcfg.elf"
POWER="$BUILD_DIR/power/power.elf"
IDENTITY="$BUILD_DIR/identity/identity.elf"
USER_CMD="$BUILD_DIR/user/user.elf"
SHUTDOWN="$BUILD_DIR/shutdown/shutdown.elf"
REBOOT="$BUILD_DIR/reboot/reboot.elf"
VERSION="$BUILD_DIR/version/version.elf"
WHERE="$BUILD_DIR/where/where.elf"

if [ "$FRESH" -eq 0 ] && [ -f "$ROOT_IMAGE" ]; then
    python3 tools/check_mgfs_version.py "$ROOT_IMAGE"
fi

mkdir -p "$BUILD_DIR/Mangrove"
mkdir -p "$(dirname "$ROOT_IMAGE")"

# Keep the ordered payload list in one place for both fresh population and
# incremental updates.  The Python tools validate its length against their
# shared canonical manifest.
run_mgfs_tool() {
    tool="$1"
    shift
    python3 "tools/${tool}.py" "$ROOT_IMAGE" \
        "$PITH" "$SPROUT" "$SESSIOND" "$LOGIND" "$LOGD" "$NETWORKD" \
        "$DEVICED" "$VOLUMED" "$CLIPD" "$LSPCI" "$LSUSB" "$LSDISK" "$CREW" "$MEM" "$TIME" "$TMON" \
        "$LOGV" "$MOUNT" "$UNMOUNT" "$EJECT" "$DISKUTIL" \
        "$SHOOT" "$CLEAR" "$COPY" "$SAY" "$UPTIME" "$LIST" "$LOCATE" \
        "$MOVE" "$PLANT" "$TYPE" "$WAIT" "$DEL" "$VERSION" "$WHERE" "$PING" \
        "$RESOLVE" "$FETCH" "$NETINFO" "$NETCFG" "$SHUTDOWN" \
        "$REBOOT" "$POWER" \
        "$IDENTITY" "$USER_CMD" "$MKDIR" "$SPROUT_CMD" "$DATE" "$ABOUT" "$REED" "$VIEW" "$INFO" \
        "$PCI_IDS" "$USB_IDS" "$HARDWARE_README" \
        "$@"
}

rm -f "$BOOT_IMAGE"

# A zero-count seek creates the same sparse 64 MiB image with GNU or BSD dd.
dd if=/dev/zero of="$BOOT_IMAGE" bs=1 count=0 seek=67108864 2>/dev/null
mkfs.fat -F32 "$BOOT_IMAGE"

mmd -i "$BOOT_IMAGE" ::/EFI
mmd -i "$BOOT_IMAGE" ::/EFI/BOOT

mcopy -i "$BOOT_IMAGE" "$BUILD_DIR/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/

if [ "$FRESH" -eq 1 ]; then
    echo "[IMAGE] Resetting MGFS image: $ROOT_IMAGE"
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
