#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Headless physical-media launcher for the one disposable SanDisk Cruzer Blade
# approved for Stage 19.14i validation.  This script never opens /dev/sdX and
# never accepts QEMU arguments.  The only privileged calls are the fixed
# root-owned bind/unbind helper installed from physical_sandisk_usbctl.sh.

set -euo pipefail
IFS=$' \t\n'
PATH=/usr/local/bin:/usr/bin:/bin

readonly TARGET_VENDOR=0781
readonly TARGET_PRODUCT=5567
readonly TARGET_SERIAL=03024220120821232925
readonly TARGET_MANUFACTURER=SanDisk
readonly TARGET_MODEL='Cruzer Blade'
readonly MIN_CAPACITY_BYTES=$((14 * 1024 * 1024 * 1024))
readonly MAX_CAPACITY_BYTES=$((16 * 1024 * 1024 * 1024))
readonly HELPER=/usr/local/libexec/mangrove-physical-sandisk-usbctl

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=$ROOT/build/Mangrove/MangroveUSB.img
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
readonly QEMU=qemu-system-x86_64

die()
{
    printf 'physical_sandisk_test: %s\n' "$*" >&2
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
    local block_path block_name device_path sectors bytes model selected_bytes
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
        selected_bytes=$bytes
    done
    ((${#matches[@]} == 1)) || die 'the approved USB does not expose one removable SanDisk block disk'
    TARGET_BLOCK_DISK=${matches[0]}
    TARGET_CAPACITY_BYTES=$selected_bytes
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

# Desktop automount may claim the disposable stick after a prior guest exits.
# Release only its currently revalidated partitions through the normal
# unprivileged UDisks interface; never invoke umount on a guessed /dev/sdX.
release_desktop_mounts()
{
    local path type mountpoint
    local -a mounted=()

    find_target_usb
    find_target_block_disk
    assert_not_host_critical
    while read -r path type; do
        [[ $type == part ]] || continue
        mountpoint=$(findmnt -nro TARGET -S "$path" 2>/dev/null || true)
        [[ -n $mountpoint ]] && mounted+=("$path")
    done < <(lsblk -nrpo NAME,TYPE "/dev/$TARGET_BLOCK_DISK" 2>/dev/null || true)
    ((${#mounted[@]})) || return 0
    command -v udisksctl >/dev/null ||
        die 'the approved USB is mounted and udisksctl is unavailable to release it safely'
    for path in "${mounted[@]}"; do
        # A desktop automounter can release the mount between findmnt and
        # UDisks.  Treat that specific race as success only after checking
        # that the same revalidated partition truly is no longer mounted.
        if ! udisksctl unmount -b "$path" >/dev/null 2>&1; then
            mountpoint=$(findmnt -nro TARGET -S "$path" 2>/dev/null || true)
            [[ -z $mountpoint ]] ||
                die "could not safely unmount the revalidated test partition $path"
        fi
    done
    # Re-identify after UDisks activity: the authoritative helper will repeat
    # this once more immediately before the privileged driver detach.
    find_target_usb
    find_target_block_disk
    assert_not_host_critical
    assert_unmounted
}

preflight()
{
    find_target_usb
    find_target_block_disk
    assert_not_host_critical
    assert_unmounted
    printf 'USB node: %s\n' "$TARGET_USB_NAME"
    printf 'USB bus/address: %s/%s\n' "$TARGET_USB_BUS" "$TARGET_USB_ADDRESS"
    printf 'Host block disk: %s (%s bytes)\n' "$TARGET_BLOCK_DISK" "$TARGET_CAPACITY_BYTES"
}

assert_usbfs_access()
{
    local usbfs_node

    printf -v usbfs_node '/dev/bus/usb/%03d/%03d' \
        "$((10#$TARGET_USB_BUS))" "$((10#$TARGET_USB_ADDRESS))"
    [[ -r $usbfs_node && -w $usbfs_node ]] ||
        die "no read/write access to $usbfs_node; install the exact udev rule first"
}

require_installed_helper()
{
    [[ -x $HELPER ]] || die "missing $HELPER; install the one-time physical-test helper first"
}

run_is_safe()
{
    local run=$1
    [[ $run == /tmp/mangrove-physical-sandisk.* && -d $run ]] || return 1
}

qemu_pid_is_current()
{
    local pid=$1 run=$2 command
    [[ $pid =~ ^[1-9][0-9]*$ && -r /proc/$pid/cmdline ]] || return 1
    command=$(tr '\0' ' ' </proc/$pid/cmdline)
    [[ $command == *"$run/monitor.sock"* && $command == *qemu-system-x86_64* ]]
}

assert_no_active_run()
{
    local pid_file pid run
    for pid_file in /tmp/mangrove-physical-sandisk.*/qemu.pid; do
        [[ -r $pid_file ]] || continue
        run=${pid_file%/qemu.pid}
        pid=$(<"$pid_file")
        qemu_pid_is_current "$pid" "$run" &&
            die "an existing physical test is still running: $run"
    done
    return 0
}

write_trace_events()
{
    local output=$1
    printf '%s\n' \
        usb_host_req_data \
        usb_host_req_complete \
        usb_host_req_canceled \
        usb_host_reset \
        usb_host_set_interface >"$output"
}

reap()
{
    local run=$1 pid=$2
    run_is_safe "$run" || exit 1
    while qemu_pid_is_current "$pid" "$run"; do
        sleep 1
    done
    sudo -n "$HELPER" attach >>"$run/reaper.log" 2>&1 ||
        printf 'physical_sandisk_test: automatic host-driver reattach failed\n' >>"$run/reaper.log"
    # A desktop automounter may race the restored driver.  Keep the approved
    # disposable device released when it is still present; an unplugged device
    # simply leaves this best-effort cleanup with no persistent state change.
    sleep 1
    release_desktop_mounts >>"$run/reaper.log" 2>&1 || true
}

start()
{
    local run qemu_pid

    [[ $EUID != 0 ]] || die 'must run unprivileged so the monitor cannot control root QEMU'
    command -v "$QEMU" >/dev/null || die 'qemu-system-x86_64 is unavailable'
    command -v setsid >/dev/null || die 'setsid is unavailable'
    command -v socat >/dev/null || die 'socat is unavailable'
    [[ -r $IMAGE ]] || die "missing rebuilt image: $IMAGE"
    [[ -r $OVMF_CODE && -r $OVMF_VARS ]] || die 'OVMF 4M firmware is unavailable'
    assert_no_active_run
    require_installed_helper
    release_desktop_mounts
    preflight
    assert_usbfs_access

    # The root helper repeats this preflight immediately before it unbinds the
    # interface.  Neither /dev/sdX nor a saved bus/address is an authority.
    sudo -n "$HELPER" detach

    umask 077
    run=$(mktemp -d /tmp/mangrove-physical-sandisk.XXXXXX)
    cp "$OVMF_VARS" "$run/OVMF_VARS.fd"
    write_trace_events "$run/qemu-usb.events"

    # The launcher normally runs from a short-lived command wrapper.  Put
    # QEMU in a distinct session so wrapper cleanup cannot terminate the
    # user-owned headless guest or its monitor socket.
    setsid "$QEMU" \
        -machine q35 \
        -accel kvm -cpu host \
        -m 512M \
        -rtc base=utc,clock=vm \
        -display none \
        -serial "file:$run/serial.log" \
        -monitor "unix:$run/monitor.sock,server=on,wait=off" \
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
        -drive "if=pflash,format=raw,file=$run/OVMF_VARS.fd" \
        -drive "id=usb,file=$IMAGE,format=raw,if=none" \
        -netdev user,id=net0 \
        -device e1000,netdev=net0,mac=52:54:00:18:01:01 \
        -device qemu-xhci,id=xhci \
        -device usb-storage,id=boot-storage,bus=xhci.0,port=2,drive=usb,bootindex=1 \
        -device usb-kbd,id=boot-kbd,bus=xhci.0,port=1 \
        -device "usb-host,id=physical-sandisk,bus=xhci.0,port=3,hostbus=$TARGET_USB_BUS,hostaddr=$TARGET_USB_ADDRESS,pipeline=off" \
        -trace "events=$run/qemu-usb.events,file=$run/qemu-usb.trace" \
        >"$run/qemu.log" 2>&1 < /dev/null &
    qemu_pid=$!
    printf '%s\n' "$qemu_pid" >"$run/qemu.pid"
    sleep 1
    if ! qemu_pid_is_current "$qemu_pid" "$run"; then
        sudo -n "$HELPER" attach || true
        die "QEMU did not stay running; see $run/qemu.log"
    fi
    setsid "$0" __reap "$run" "$qemu_pid" >>"$run/reaper.log" 2>&1 < /dev/null &
    printf 'QEMU IS RUNNING\nRUN_DIR=%s\n' "$run"
}

stop()
{
    local run=$1 pid index
    run_is_safe "$run" || die 'invalid RUN_DIR'
    [[ -r $run/qemu.pid ]] || die 'RUN_DIR has no QEMU pid'
    pid=$(<"$run/qemu.pid")
    if [[ -S $run/monitor.sock ]]; then
        printf 'quit\n' | socat - UNIX-CONNECT:"$run/monitor.sock" >/dev/null 2>&1 || true
    fi
    for index in $(seq 1 20); do
        qemu_pid_is_current "$pid" "$run" || break
        sleep 1
    done
    qemu_pid_is_current "$pid" "$run" && die 'QEMU did not exit after monitor quit'
    sudo -n "$HELPER" attach
    sleep 1
    release_desktop_mounts
    printf 'Host test device reattached and left unmounted.\n'
}

usage()
{
    cat <<'EOF'
usage: scripts/physical_sandisk_test.sh <preflight|release|start|stop RUN_DIR|recover>

The launcher only targets the one approved SanDisk Cruzer Blade.  start is
headless and prints a fresh RUN_DIR with a user-owned monitor socket.
EOF
}

case ${1:-} in
    preflight)
        preflight
        ;;
    release)
        [[ $# == 1 ]] || die 'release accepts no arguments'
        release_desktop_mounts
        preflight
        printf 'Host test device is unmounted.\n'
        ;;
    start)
        [[ $# == 1 ]] || die 'start accepts no QEMU or device arguments'
        start
        ;;
    stop)
        [[ $# == 2 ]] || die 'usage: stop RUN_DIR'
        stop "$2"
        ;;
    recover)
        [[ $# == 1 ]] || die 'recover accepts no arguments'
        assert_no_active_run
        sudo -n "$HELPER" attach
        ;;
    __reap)
        [[ $# == 3 ]] || exit 1
        reap "$2" "$3"
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
