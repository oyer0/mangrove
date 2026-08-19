#!/bin/sh
set -e

BOOT_IMAGE=build/Mangrove/Boot.img
ROOT_IMAGE=build/Mangrove/Mangrove.img
LEGACY_IMAGE=build/Mangrove/TestDisk.img
MKMGFS=build/mkmgfs
SPROUT=build/Sprout/sprout.elf
HELLO=build/Hello/hello.elf
SHOOT=build/Shoot/shoot.elf
FSTEST=build/FsTest/fstest.elf
COPY=build/Copy/copy.elf
SAY=build/Say/say.elf
UPTIME=build/Uptime/uptime.elf
NETTEST=build/NetTest/nettest.elf
PING=build/Ping/ping.elf
RESOLVE=build/Resolve/resolve.elf
FETCH=build/Fetch/fetch.elf
NETWORK=build/Network/network.elf
FRESH=0

if [ "${1:-}" = "--fresh" ]; then
    FRESH=1
elif [ "${1:-}" != "" ]; then
    echo "Usage: $0 [--fresh]" >&2
    exit 2
fi

mkdir -p build/Mangrove

rm -f "$BOOT_IMAGE"

truncate -s 64M "$BOOT_IMAGE"
mkfs.fat -F32 "$BOOT_IMAGE"

mmd -i "$BOOT_IMAGE" ::/EFI
mmd -i "$BOOT_IMAGE" ::/EFI/BOOT
mmd -i "$BOOT_IMAGE" ::/Mangrove

mcopy -i "$BOOT_IMAGE" build/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/
mcopy -i "$BOOT_IMAGE" build/Mangrove/kernel.elf ::/Mangrove/

if [ "$FRESH" -eq 1 ]; then
    rm -f "$ROOT_IMAGE" "$LEGACY_IMAGE"
elif [ ! -f "$ROOT_IMAGE" ] && [ -f "$LEGACY_IMAGE" ]; then
    echo "Migrating legacy MGFS root image $LEGACY_IMAGE to $ROOT_IMAGE"
    mv "$LEGACY_IMAGE" "$ROOT_IMAGE"
fi

if [ -f "$ROOT_IMAGE" ] && ! head -c 8 "$ROOT_IMAGE" | grep -a -q 'MGFSv1'; then
    if [ -f "$LEGACY_IMAGE" ] && head -c 8 "$LEGACY_IMAGE" | grep -a -q 'MGFSv1'; then
        echo "Replacing legacy FAT root image with $LEGACY_IMAGE"
        rm -f "$ROOT_IMAGE"
        mv "$LEGACY_IMAGE" "$ROOT_IMAGE"
    else
        echo "Discarding obsolete non-MGFS root image $ROOT_IMAGE"
        rm -f "$ROOT_IMAGE"
    fi
fi

if [ ! -f "$ROOT_IMAGE" ]; then
    echo "Creating fresh MGFS root image $ROOT_IMAGE..."
    "$MKMGFS" \
        --blocks 16384 \
        --uuid 00000000-0000-0000-0000-000000000001 \
        --format-time-ns 0 \
        "$ROOT_IMAGE"
fi

python3 tools/populate_mgfs.py "$ROOT_IMAGE" "$SPROUT" "$SHOOT" "$COPY" "$SAY" "$UPTIME" "$HELLO" "$FSTEST" "$NETTEST" "$PING" "$RESOLVE" "$FETCH" "$NETWORK"
