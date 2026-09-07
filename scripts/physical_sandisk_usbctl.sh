#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Source for a root-owned, narrowly scoped helper used by
# physical_sandisk_test.sh.  It deliberately has no QEMU, shell, path, or
# device-name arguments: it can only bind or unbind the one disposable USB
# stick identified below.

set -euo pipefail
IFS=$' \t\n'
PATH=/usr/sbin:/usr/bin:/sbin:/bin

readonly TARGET_VENDOR=0781
readonly TARGET_PRODUCT=5567
readonly TARGET_SERIAL=03024220120821232925
readonly TARGET_MANUFACTURER=SanDisk
readonly TARGET_MODEL='Cruzer Blade'
readonly MIN_CAPACITY_BYTES=$((14 * 1024 * 1024 * 1024))
readonly MAX_CAPACITY_BYTES=$((16 * 1024 * 1024 * 1024))

die()
{
    printf 'mangrove-physical-usbctl: %s\n' "$*" >&2
    exit 1
}

read_attr()
{
    local path=$1
    [[ -r $path ]] || return 1
    tr -d '\n' <"$path"
}

find_target_usb()
{
    local node
    local -a matches=()

    for node in /sys/bus/usb/devices/*; do
        [[ -r $node/idVendor && -r $node/idProduct && -r $node/serial ]] || continue
        [[ $(read_attr "$node/idVendor") == "$TARGET_VENDOR" ]] || continue
        [[ $(read_attr "$node/idProduct") == "$TARGET_PRODUCT" ]] || continue
        [[ $(read_attr "$node/serial") == "$TARGET_SERIAL" ]] || continue
        matches+=("$node")
    done
    ((${#matches[@]} == 1)) || die 'the uniquely identified SanDisk is not present'

    TARGET_USB_NODE=${matches[0]}
    TARGET_USB_NAME=${TARGET_USB_NODE##*/}
    TARGET_USB_REAL=$(readlink -f "$TARGET_USB_NODE") || die 'cannot resolve USB sysfs node'
    [[ $(read_attr "$TARGET_USB_NODE/manufacturer") == "$TARGET_MANUFACTURER" ]] ||
        die 'USB manufacturer does not match the approved SanDisk'
    [[ $(read_attr "$TARGET_USB_NODE/product") == "$TARGET_MODEL" ]] ||
        die 'USB product does not match the approved Cruzer Blade'
    TARGET_USB_BUS=$(read_attr "$TARGET_USB_NODE/busnum") || die 'USB bus is unavailable'
    TARGET_USB_ADDRESS=$(read_attr "$TARGET_USB_NODE/devnum") || die 'USB address is unavailable'
}

find_target_block_disk()
{
    local block_path block_name device_path sectors bytes model
    local -a matches=()

    for block_path in /sys/block/sd*; do
        [[ -e $block_path/device ]] || continue
        device_path=$(readlink -f "$block_path/device") || continue
        case $device_path in
            "$TARGET_USB_REAL"/*) ;;
            *) continue ;;
        esac
        block_name=${block_path##*/}
        [[ $(read_attr "$block_path/removable") == 1 ]] || continue
        sectors=$(read_attr "$block_path/size") || continue
        [[ $sectors =~ ^[0-9]+$ ]] || continue
        bytes=$((sectors * 512))
        ((bytes >= MIN_CAPACITY_BYTES && bytes <= MAX_CAPACITY_BYTES)) || continue
        model=$(read_attr "$block_path/device/model" | sed 's/[[:space:]]*$//') || continue
        [[ $model == "$TARGET_MODEL" ]] || continue
        matches+=("$block_name")
    done
    ((${#matches[@]} == 1)) || die 'the approved USB does not expose one removable SanDisk block disk'
    TARGET_BLOCK_DISK=${matches[0]}
}

assert_not_host_critical()
{
    local source label ancestor
    local -a sources=()

    source=$(findmnt -nro SOURCE /) || die 'cannot identify the host root source'
    sources+=("root:$source")
    for label in boot efi; do
        case $label in
            boot) source=$(findmnt -nro SOURCE /boot 2>/dev/null || true) ;;
            efi) source=$(findmnt -nro SOURCE /boot/efi 2>/dev/null || true) ;;
        esac
        [[ -n $source ]] && sources+=("$label:$source")
    done
    while IFS= read -r source; do
        [[ -n $source ]] && sources+=("swap:$source")
    done < <(swapon --noheadings --raw --output NAME 2>/dev/null || true)

    for label in "${sources[@]}"; do
        source=${label#*:}
        [[ $source == /dev/* ]] || die "host ${label%%:*} source is not a block device"
        while IFS= read -r ancestor; do
            [[ ${ancestor##*/} != "$TARGET_BLOCK_DISK" ]] ||
                die "refusing: approved USB backs host ${label%%:*}"
        done < <(lsblk -s -nr -o NAME "$source" 2>/dev/null || true)
    done
}

assert_unmounted()
{
    local mountpoint
    while IFS= read -r mountpoint; do
        [[ -z $mountpoint ]] || die "refusing: /dev/$TARGET_BLOCK_DISK has mounted filesystems"
    done < <(lsblk -nr -o MOUNTPOINT "/dev/$TARGET_BLOCK_DISK" 2>/dev/null || true)
}

find_mass_storage_interface()
{
    local interface class driver
    local -a matches=()

    for interface in /sys/bus/usb/devices/"$TARGET_USB_NAME":*; do
        [[ -r $interface/bInterfaceClass ]] || continue
        class=$(read_attr "$interface/bInterfaceClass")
        [[ $class == 08 ]] || continue
        matches+=("$interface")
    done
    ((${#matches[@]} == 1)) || die 'the approved USB does not expose one mass-storage interface'
    TARGET_INTERFACE=${matches[0]}
    TARGET_INTERFACE_NAME=${TARGET_INTERFACE##*/}
    # Do not canonicalize this link.  GNU readlink -f fabricates a pathname
    # ending in "driver" when the interface is intentionally unbound.  The
    # symlink target names the bound driver; an absent link is expected.
    driver=$(readlink "$TARGET_INTERFACE/driver" 2>/dev/null || true)
    TARGET_INTERFACE_DRIVER=${driver##*/}
}

validate_detach_target()
{
    find_target_usb
    find_target_block_disk
    assert_not_host_critical
    assert_unmounted
    find_mass_storage_interface
}

validate_attach_target()
{
    find_target_usb
    find_mass_storage_interface
}

detach()
{
    validate_detach_target
    case $TARGET_INTERFACE_DRIVER in
        usb-storage)
            printf '%s' "$TARGET_INTERFACE_NAME" > /sys/bus/usb/drivers/usb-storage/unbind
            ;;
        '')
            die 'the approved USB mass-storage interface is already unbound'
            ;;
        *)
            die "refusing: approved USB is bound to unexpected driver $TARGET_INTERFACE_DRIVER"
            ;;
    esac
}

attach()
{
    validate_attach_target
    case $TARGET_INTERFACE_DRIVER in
        usb-storage)
            exit 0
            ;;
        '')
            printf '%s' "$TARGET_INTERFACE_NAME" > /sys/bus/usb/drivers/usb-storage/bind
            ;;
        *)
            die "refusing: approved USB is bound to unexpected driver $TARGET_INTERFACE_DRIVER"
            ;;
    esac
}

[[ ${EUID:-} == 0 ]] || die 'must run as root'
[[ $# == 1 ]] || die 'usage: mangrove-physical-sandisk-usbctl <detach|attach>'
case $1 in
    detach) detach ;;
    attach) attach ;;
    *) die 'usage: mangrove-physical-sandisk-usbctl <detach|attach>' ;;
esac
