#!/bin/sh
set -e

IMAGE=build/Mangrove/Mangrove.img
TEST_IMAGE=build/Mangrove/TestDisk.img

mkdir -p build/Mangrove

rm -f "$IMAGE"

truncate -s 64M "$IMAGE"
mkfs.fat -F32 "$IMAGE"

mmd -i "$IMAGE" ::/EFI
mmd -i "$IMAGE" ::/EFI/BOOT
mmd -i "$IMAGE" ::/Mangrove

mcopy -i "$IMAGE" build/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/
mcopy -i "$IMAGE" build/Mangrove/kernel.elf ::/Mangrove/

if [ ! -f "$TEST_IMAGE" ]; then
    echo "Creating $TEST_IMAGE..."
    truncate -s 64M "$TEST_IMAGE"
    mkfs.fat -F32 "$TEST_IMAGE"
fi
