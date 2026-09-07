#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -e

DISK_IMAGE=.mangrove/MangroveDev.img
ROOT_IMAGE=.mangrove/MangroveDevRoot.img
FRESH=0
DISK_CREATED=0

DISK_SECTORS=264225
DISK_BYTES=$((DISK_SECTORS * 512))
LEGACY_ROOT=build/Mangrove/Mangrove.img

while [ "$#" -gt 0 ]; do
    case "$1" in
        --disk)
            shift
            [ "$#" -gt 0 ] || { echo "Usage: $0 [--fresh] --disk image --root image" >&2; exit 2; }
            DISK_IMAGE=$1
            ;;
        --root)
            shift
            [ "$#" -gt 0 ] || { echo "Usage: $0 [--fresh] --disk image --root image" >&2; exit 2; }
            ROOT_IMAGE=$1
            ;;
        --fresh)
            FRESH=1
            ;;
        *)
            echo "Usage: $0 [--fresh] --disk image --root image" >&2
            exit 2
            ;;
    esac
    shift
done

mkdir -p "$(dirname "$DISK_IMAGE")" "$(dirname "$ROOT_IMAGE")"

if [ -f "$DISK_IMAGE" ]; then
    # QEMU takes an advisory lock on raw images.  Fail clearly instead of
    # allowing a second developer run to block in an image write.
    if command -v fuser >/dev/null 2>&1; then
        if fuser "$DISK_IMAGE" >/dev/null 2>&1; then
            echo "Persistent development disk is in use: $DISK_IMAGE" >&2
            echo "Stop the existing Mangrove run before updating it." >&2
            exit 1
        fi
    elif command -v lsof >/dev/null 2>&1 &&
         lsof -t "$DISK_IMAGE" >/dev/null 2>&1; then
        echo "Persistent development disk is in use: $DISK_IMAGE" >&2
        echo "Stop the existing Mangrove run before updating it." >&2
        exit 1
    fi
fi

if [ "$FRESH" -eq 1 ]; then
    echo "[FRESH] Removing persistent development disk: $DISK_IMAGE"
    rm -f "$DISK_IMAGE" "$ROOT_IMAGE"
fi

# Migrate the old raw MGFS development image once.  The new persistent disk
# is a complete GPT disk so QEMU and physical USB use the same topology.
if [ "$FRESH" -eq 0 ] && [ ! -f "$ROOT_IMAGE" ] && [ -f "$DISK_IMAGE" ] && head -c 8 "$DISK_IMAGE" | grep -a -q 'MGFSv1'; then
    echo "[DEV] Migrating raw MGFS image into $ROOT_IMAGE"
    cp "$DISK_IMAGE" "$ROOT_IMAGE"
    rm -f "$DISK_IMAGE"
fi
if [ "$FRESH" -eq 0 ] && [ ! -f "$ROOT_IMAGE" ] && [ -f "$LEGACY_ROOT" ]; then
    echo "[DEV] Migrating legacy MGFS image into $ROOT_IMAGE"
    cp "$LEGACY_ROOT" "$ROOT_IMAGE"
fi

if [ -f "$DISK_IMAGE" ]; then
    if [ "$(wc -c < "$DISK_IMAGE")" -ne "$DISK_BYTES" ]; then
        echo "Invalid persistent development disk size: $DISK_IMAGE" >&2
        exit 1
    fi
    python3 tools/copy_partition.py extract \
        --disk "$DISK_IMAGE" --root "$ROOT_IMAGE"
fi

if [ "$FRESH" -eq 1 ]; then
    ./scripts/make_image.sh --fresh --root "$ROOT_IMAGE" --autologin developer
elif [ -f "$ROOT_IMAGE" ]; then
    ./scripts/make_image.sh --root "$ROOT_IMAGE" --autologin developer
else
    ./scripts/make_image.sh --fresh --root "$ROOT_IMAGE" --autologin developer
fi

if [ ! -f "$DISK_IMAGE" ]; then
    DISK_CREATED=1
    dd if=/dev/zero of="$DISK_IMAGE" bs=1 count=0 seek="$DISK_BYTES" 2>/dev/null
    if [ "$(uname -s)" = Darwin ]; then
        sgdisk --zap-all \
            --new=1:2048:133119 --typecode=1:EF00 --change-name=1:MANGROVE_ESP \
            --new=2:133120:264191 --typecode=2:8300 --change-name=2:MANGROVE_ROOT \
            "$DISK_IMAGE" >/dev/null 2>&1
    else
        parted -s -a minimal "$DISK_IMAGE" mklabel gpt
        parted -s -a minimal "$DISK_IMAGE" mkpart MANGROVE_ESP fat32 2048s 133119s
        parted -s -a minimal "$DISK_IMAGE" set 1 esp on
        parted -s -a minimal "$DISK_IMAGE" mkpart MANGROVE_ROOT 133120s 264191s
    fi
else
    # Give existing development disks the explicit role markers used by the
    # kernel.  This is an idempotent GPT metadata migration, not a data move.
    if [ "$(uname -s)" = Darwin ]; then
        sgdisk --change-name=1:MANGROVE_ESP \
               --change-name=2:MANGROVE_ROOT "$DISK_IMAGE" >/dev/null 2>&1
    else
        parted -s -a minimal "$DISK_IMAGE" name 1 MANGROVE_ESP
        parted -s -a minimal "$DISK_IMAGE" name 2 MANGROVE_ROOT
    fi
fi

# A new disk can receive the freshly-created ESP image in one write.  For an
# existing development disk, update only Mangrove's loader so unrelated ESP
# files survive an incremental image update.
if [ "$DISK_CREATED" -eq 1 ]; then
    dd if=build/Mangrove/Boot.img of="$DISK_IMAGE" bs=512 seek=2048 conv=notrunc 2>/dev/null
else
    ESP_IMAGE="$DISK_IMAGE@@1048576"
    # mmd asks its controlling terminal how to handle an existing directory.
    # The persistent ESP normally already contains these directories, so probe
    # them first instead of relying on an ignored mmd failure.
    ensure_esp_directory() {
        path="$1"
        if mdir -i "$ESP_IMAGE" "${path}/" >/dev/null 2>&1; then
            return 0
        fi

        mmd -i "$ESP_IMAGE" "$path"
        mdir -i "$ESP_IMAGE" "${path}/" >/dev/null 2>&1
    }

    ensure_esp_directory ::/EFI
    ensure_esp_directory ::/EFI/BOOT
    mcopy -o -i "$ESP_IMAGE" build/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/ >/dev/null
    for old_path in \
        ::/MANGROVE/kernel.elf \
        ::/MANGROVE/KERNEL.ELF \
        ::/MANGROVE/rhizome.elf \
        ::/MANGROVE/RHIZOME.ELF \
        ::/MANGROVE/pith.elf; do
        mdel -i "$ESP_IMAGE" "$old_path" 2>/dev/null || true
    done
    # Remove the former payload-only directory when it is empty.  mrd fails
    # harmlessly if the directory contains unrelated firmware files.
    mrd -i "$ESP_IMAGE" ::/MANGROVE 2>/dev/null || true
fi
python3 tools/copy_partition.py write \
    --disk "$DISK_IMAGE" --root "$ROOT_IMAGE"
