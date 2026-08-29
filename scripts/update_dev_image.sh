#!/bin/sh
set -e

DISK_IMAGE=.mangrove/MangroveDev.img
ROOT_IMAGE=.mangrove/MangroveDevRoot.img
FRESH=0

ROOT_SECTOR=133120
ROOT_SECTORS=131072
DISK_BYTES=135283200
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
    dd if="$DISK_IMAGE" of="$ROOT_IMAGE" bs=512 skip="$ROOT_SECTOR" count="$ROOT_SECTORS" 2>/dev/null
fi

if [ "$FRESH" -eq 1 ]; then
    ./scripts/make_image.sh --fresh --root "$ROOT_IMAGE" --autologin developer
elif [ -f "$ROOT_IMAGE" ]; then
    ./scripts/make_image.sh --root "$ROOT_IMAGE" --autologin developer
else
    ./scripts/make_image.sh --fresh --root "$ROOT_IMAGE" --autologin developer
fi

if [ ! -f "$DISK_IMAGE" ]; then
    dd if=/dev/zero of="$DISK_IMAGE" bs=1 count=0 seek="$DISK_BYTES" 2>/dev/null
    if [ "$(uname -s)" = Darwin ]; then
        sgdisk --zap-all \
            --new=1:2048:133119 --typecode=1:EF00 --change-name=1:ESP \
            --new=2:133120:264191 --typecode=2:8300 --change-name=2:primary \
            "$DISK_IMAGE" >/dev/null 2>&1
    else
        parted -s -a minimal "$DISK_IMAGE" mklabel gpt
        parted -s -a minimal "$DISK_IMAGE" mkpart ESP fat32 2048s 133119s
        parted -s -a minimal "$DISK_IMAGE" set 1 esp on
        parted -s -a minimal "$DISK_IMAGE" mkpart primary 133120s 264191s
    fi
fi

dd if=build/Mangrove/Boot.img of="$DISK_IMAGE" bs=512 seek=2048 conv=notrunc 2>/dev/null
dd if="$ROOT_IMAGE" of="$DISK_IMAGE" bs=512 seek="$ROOT_SECTOR" conv=notrunc 2>/dev/null
