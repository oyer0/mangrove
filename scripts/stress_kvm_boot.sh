#!/usr/bin/env bash
# Repeated cold-boot probe for an already-built Mangrove USB image.
#
# This deliberately does not invoke make or rebuild an image.  It keeps the
# normal run-usb device topology, including E1000/user networking and xHCI,
# and stores one guest serial log plus one framebuffer screendump per run so
# early panics remain inspectable even when the graphical console is sparse.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
image=${MANGROVE_USB_IMAGE:-"$root/build/Mangrove/MangroveUSB.img"}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
qemu=${QEMU:-qemu-system-x86_64}
runs=${1:-20}
seconds=${BOOT_SECONDS:-25}
output=${BOOT_STRESS_OUTPUT:-"$root/build/boot-stress"}

if ! [[ $runs =~ ^[1-9][0-9]*$ ]] || ! [[ $seconds =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [positive-run-count]" >&2
    exit 2
fi
if [[ ! -f $image || ! -f $ovmf_code || ! -f $ovmf_vars ]]; then
    echo "missing existing image or OVMF firmware; this script does not build them" >&2
    exit 2
fi
if ! test -r /dev/kvm || ! test -w /dev/kvm; then
    echo "/dev/kvm is not accessible" >&2
    exit 2
fi
if ! command -v socat >/dev/null 2>&1; then
    echo "socat is required to request QEMU screendumps" >&2
    exit 2
fi

mkdir -p "$output"

for run in $(seq 1 "$runs"); do
    run_dir="$output/run-$(printf '%03d' "$run")"
    monitor="$run_dir/monitor.sock"
    vars="$run_dir/OVMF_VARS.fd"
    mkdir -p "$run_dir"
    cp "$ovmf_vars" "$vars"

    "$qemu" \
        -machine q35 \
        -accel kvm -cpu host \
        -m 512M \
        -display none \
        -serial "file:$run_dir/guest.log" \
        -monitor "unix:$monitor,server=on,wait=off" \
        -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "id=usb,file=$image,format=raw,if=none" \
        -netdev user,id=net0 \
        -device e1000,netdev=net0,mac=52:54:00:18:01:01 \
        -device qemu-xhci,id=xhci \
        -device usb-storage,bus=xhci.0,port=2,drive=usb,bootindex=1 \
        -device usb-kbd,bus=xhci.0,port=1 \
        >"$run_dir/qemu.log" 2>&1 &
    qemu_pid=$!

    sleep "$seconds"
    if [[ -S $monitor ]]; then
        printf 'screendump %s\nquit\n' "$run_dir/screen.ppm" |
            socat - "UNIX-CONNECT:$monitor" >"$run_dir/monitor.log" 2>&1 || true
    fi
    if kill -0 "$qemu_pid" 2>/dev/null; then
        kill "$qemu_pid" 2>/dev/null || true
    fi
    wait "$qemu_pid" 2>/dev/null || true
    echo "boot $run/$runs: $run_dir"
done
