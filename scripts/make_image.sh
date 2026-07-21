#!/bin/sh
set -e

IMAGE=build/Mangrove/Mangrove.img

rm -f "$IMAGE"

truncate -s 64M "$IMAGE"
mkfs.fat -F32 "$IMAGE"

mmd -i "$IMAGE" ::/EFI
mmd -i "$IMAGE" ::/EFI/BOOT
mmd -i "$IMAGE" ::/Mangrove

mcopy -i "$IMAGE" build/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/
mcopy -i "$IMAGE" build/Mangrove/kernel.elf ::/Mangrove/
