#!/bin/sh
set -e

BOOT_IMAGE=build/Mangrove/Boot.img
ROOT_IMAGE=build/Mangrove/Mangrove.img
MKMGFS=build/mkmgfs
SPROUT=build/Sprout/sprout.elf
SHOOT=build/Shoot/shoot.elf
CLEAR=build/Clear/clear.elf
COPY=build/Copy/copy.elf
LIST=build/List/list.elf
LOCATE=build/Locate/locate.elf
MOVE=build/Move/move.elf
PLANT=build/Plant/plant.elf
READ=build/Read/read.elf
REMOVE=build/Remove/remove.elf
SAY=build/Say/say.elf
UPTIME=build/Uptime/uptime.elf
PING=build/Ping/ping.elf
RESOLVE=build/Resolve/resolve.elf
FETCH=build/Fetch/fetch.elf
NETWORK=build/Network/network.elf
POWER=build/Power/power.elf
SHUTDOWN=build/Shutdown/shutdown.elf
REBOOT=build/Reboot/reboot.elf
VERSION=build/Version/version.elf
WHERE=build/Where/where.elf
FRESH=0

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
        *)
            echo "Usage: $0 [--fresh] [--root image]" >&2
            exit 2
            ;;
    esac
    shift
done

mkdir -p build/Mangrove
mkdir -p "$(dirname "$ROOT_IMAGE")"

rm -f "$BOOT_IMAGE"

# A zero-count seek creates the same sparse 64 MiB image with GNU or BSD dd.
dd if=/dev/zero of="$BOOT_IMAGE" bs=1 count=0 seek=67108864 2>/dev/null
mkfs.fat -F32 "$BOOT_IMAGE"

mmd -i "$BOOT_IMAGE" ::/EFI
mmd -i "$BOOT_IMAGE" ::/EFI/BOOT
mmd -i "$BOOT_IMAGE" ::/Mangrove

mcopy -i "$BOOT_IMAGE" build/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/
mcopy -i "$BOOT_IMAGE" build/Mangrove/kernel.elf ::/Mangrove/

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
    python3 tools/populate_mgfs.py "$ROOT_IMAGE" "$SPROUT" "$SHOOT" "$CLEAR" "$COPY" "$SAY" "$UPTIME" "$LIST" "$LOCATE" "$MOVE" "$PLANT" "$READ" "$REMOVE" "$VERSION" "$WHERE" "$PING" "$RESOLVE" "$FETCH" "$NETWORK" "$SHUTDOWN" "$REBOOT" "$POWER"
else
    python3 tools/update_mgfs.py "$ROOT_IMAGE" "$SPROUT" "$SHOOT" "$CLEAR" "$COPY" "$SAY" "$UPTIME" "$LIST" "$LOCATE" "$MOVE" "$PLANT" "$READ" "$REMOVE" "$VERSION" "$WHERE" "$PING" "$RESOLVE" "$FETCH" "$NETWORK" "$SHUTDOWN" "$REBOOT" "$POWER"
fi
