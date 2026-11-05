# ThinkPad X250 hardware profile

This document records the physical configuration used to qualify Mangrove on
the ThinkPad X250. It separates operator-confirmed physical observations from
host-test evidence. Fields without an observation are marked unknown; the
model name alone does not identify the firmware configuration.

## Current status

The physical model, BIOS and embedded-controller versions, installed devices,
and displayed ThinkPad Setup configuration are recorded from OpenBSD 8.0 and
the operator's direct firmware inspection. The operator has confirmed the
X250 recovery path in normal use: OpenBSD boots from the internal SSD, F12
selects and boots Mangrove from the SanDisk, and OpenBSD can be reached again
without changing persistent boot order. The exact compact image used for the
sustained-keyboard test remains unknown, so no retained build hash is
presented as its physical image identity.

### Operator-confirmed physical observations

- The target is a Lenovo ThinkPad X250, type `20CLS1EY00`. OpenBSD reports BIOS
  `N10ET63W` version `1.42`, dated 2021-07-29; UEFI 2.3.1 and ACPI 5.0.
  ThinkPad Setup reports Embedded Controller `N10HT17W` version `1.16` and
  Intel ME `10.0.60.3000`. The system-board revision is unknown.
- OpenBSD identifies an Intel Core i7-5600U, 8 GB RAM, Intel HD Graphics 5500
  at 1366×768, and a Samsung MZ7LN256 256 GB SATA SSD. Its CPU inventory finds
  four logical CPUs but brings two online. ThinkPad Setup reports
  Hyper-Threading enabled. OpenBSD's two-online result is its policy/result,
  not evidence that the processor or firmware exposes only two threads; see
  the CPU inventory note below before interpreting physical SMP results.
- OpenBSD identifies Intel I218-LM Ethernet, Intel Wireless-AC 7265, and an
  Intel 9 Series xHCI controller. The I218-LM is the physical Ethernet target
  for future physical Ethernet qualification. Mangrove has not qualified a
  driver for either network device on this machine; their identification alone
  says nothing about driver support.
- OpenBSD attaches the built-in keyboard as an AT Translated Set 2 keyboard
  on i8042/serio0. The Synaptics clickpad is attached through the i8042
  auxiliary input path. Mangrove's native PS/2 keyboard has been confirmed
  interactively on physical X250; clickpad support has not been qualified.
- Mangrove was launched through UEFI firmware and reached Pith. ThinkPad Setup
  is configured for both UEFI and Legacy with UEFI first; the firmware also
  reports CSM support enabled. This records the selected policy, not a claim
  that Mangrove was tested in Legacy mode.
- The operator confirms OpenBSD boots from the internal Samsung SSD with the
  SanDisk disconnected. F12 Boot Device List works and has been used to select
  and boot Mangrove from the SanDisk. The operator can return to OpenBSD
  without changing persistent boot order. This is physical operator evidence,
  not an automated recovery test; the exact firmware menu entry used to return
  to OpenBSD has not been identified.
- OpenBSD identifies the Intel 9 Series xHCI controller; earlier Mangrove
  traces recorded PCI `8086:9cb1` and EHCI `8086:9ca6`. Native xHCI routing
  and USB mass storage worked for the tested SanDisk Cruzer Blade; this does
  not imply that every USB port or peripheral is supported.
- OpenBSD reports three integrated USB peripherals at the internal topology
  locations previously called ports 6–8 in Mangrove traces: Validity Sensors
  VFS5011 fingerprint reader (`138a:0017`) on port 6, Intel Bluetooth
  (`8087:0a2a`) on port 7, and SunplusIT camera (`5986:0366`) on port 8.
  These identities strongly correlate the reported setup failures on ports 7
  and 8 with those devices, but do not establish where their setup failed or
  why. Mangrove currently has no fingerprint, Bluetooth, or camera class
  driver; these peripherals are intentionally unsupported and outside the
  current supported-device baseline. A successful device identification with
  no matching class is distinct from a real enumeration/transfer/setup
  failure. See
  [USB support policy](../architecture/usb.md).
- Earlier bring-up traces first showed the internal SATA disk with no USB mass
  storage. A later port-state report showed connection state on ports 1, 6, 7,
  and 8 only in its final sample. Video transcription of those raw values is
  provisional; the reported `0x6c1` means CCS set with PLS=Inactive, not an
  enumerable USB2 link. The final successful boot establishes USB storage
  support on the tested port. The mapping from xHCI root-hub ports to external
  connector positions has not been recorded.
- The internal storage trace identified a Samsung SATA disk. The exact PCI
  controller ID and disk firmware are unknown; the OpenBSD inventory supplied
  the model string.
- The boot USB was identified as a SanDisk Cruzer Blade, USB VID:PID
  `0781:5567`, with 30,629,376 512-byte logical sectors (15,682,240,512 bytes).
  Mangrove validated its GPT, found two partitions including `MANGROVE_ROOT`,
  mounted MGFS, and launched Shoot. The operator also confirmed filesystem
  operations in Shoot.
- The built-in keyboard is attached through i8042/serio0 as an AT Translated
  Set 2 keyboard. The operator confirmed sustained physical input at Shoot.
  The physical keymap/layout and exact image hash used for this test are
  unknown. Do not use the retained KVM-qualified candidate hash as a physical
  image identity.
- Prior traces reported runtime device setup failures on xHCI port 7
  (`CONTROLLER_BAD`, code 6) and port 8 (`INVALID_PARAM`, code 3). OpenBSD's
  topology identifies Intel Bluetooth (`8087:0a2a`) on port 7 and a SunplusIT
  camera (`5986:0366`) on port 8, strongly correlating those devices with the
  failures. The setup phase and root cause remain unknown; these are not
  reclassified as clean unsupported-class outcomes.
- OpenBSD confirms the physical Ethernet controller is Intel I218-LM. The
  earlier Mangrove observation that no Ethernet controller was supported is
  therefore a driver-coverage result, not absence of Ethernet hardware. Exact
  PCI function/revision and Mangrove link qualification are unknown.
- RTC/time warnings and incomplete physical shutdown behavior were observed,
  but the exact messages and circumstances are unknown. They are not yet
  attributed to a clock, ACPI, or power-driver defect.
- OpenBSD sensor readings at inventory time were approximately 47 °C CPU,
  50.5 °C PCH, and 2561 RPM fan. These are a one-time baseline, not a thermal
  qualification under sustained load.
- OpenBSD selected TSC as its timecounter and listed HPET and the ACPI timer as
  available. This describes OpenBSD's timer policy only; it does not establish
  Pith's timer source or accuracy.
- Physical Mangrove multicore overlap has not been qualified. OpenBSD's two
  online CPUs must not be used to infer a Mangrove CPU limit.

### OpenBSD CPU-count interpretation

OpenBSD 8.0 reports four logical CPUs discovered and two online on this
two-core/four-thread processor. OpenBSD exposes `hw.ncpufound`,
`hw.ncpuonline`, and `hw.blockcpu`; the `S` blockcpu policy excludes SMT
siblings on supported systems. The 4-found/2-online result is consistent with
that policy, but the retained inventory did not include `hw.blockcpu` or the
per-CPU `dmesg` attachment lines, so the cause is unconfirmed for this boot.
OpenBSD's [SMT FAQ](https://www.openbsd.org/faq/faq10.html#SMT) documents its
default SMT scheduling policy. ThinkPad Setup reports Hyper-Threading enabled;
that does not establish OpenBSD's CPU policy or Mangrove's online CPU set. Do
not infer a Mangrove CPU limit from OpenBSD's result; measure Mangrove's
ACPI/MP startup independently.

### Firmware profile

| Field | Current value | Source |
| --- | --- | --- |
| Exact ThinkPad type/model (MTM/SKU) | 20CLS1EY00 | OpenBSD inventory |
| BIOS version/date | N10ET63W 1.42, 2021-07-29 | OpenBSD and ThinkPad Setup |
| UEFI and ACPI versions | UEFI 2.3.1; ACPI 5.0 | OpenBSD inventory |
| Embedded Controller | N10HT17W 1.16 | ThinkPad Setup |
| Intel Management Engine | 10.0.60.3000 | ThinkPad Setup |
| UEFI/Legacy selection | Both; UEFI First; CSM Support Yes | ThinkPad Setup |
| Boot mode | Quick | ThinkPad Setup |
| Secure Boot | Disabled; Platform Mode Setup Mode; Secure Boot Mode Custom Mode | ThinkPad Setup |
| USB UEFI BIOS Support | Enabled | ThinkPad Setup |
| Always On USB | Enabled | ThinkPad Setup |
| Charge in Battery Mode | Disabled | ThinkPad Setup |
| USB 3.0 Mode | Enabled | ThinkPad Setup |
| Hyper-Threading | Enabled | ThinkPad Setup |
| F12 Boot Device List | Enabled; menu operation and Mangrove USB selection confirmed | ThinkPad Setup and operator physical report |
| Option Key Display | Enabled | ThinkPad Setup |
| Boot Order Lock | Disabled | ThinkPad Setup |
| Network Boot | PCI LAN | ThinkPad Setup |
| Historical firmware changes for Mangrove | Unknown | No prior-change record available |
| System-board revision | Unknown | Not observed |
| USB topology and sensor summary | See physical observations above | OpenBSD inventory |

### Recorded boot priority

ThinkPad Setup displayed this order:

1. Linux Boot Manager
2. Fallback Linux Boot Manager
3. Windows Boot Manager
4. USB CD
5. USB FDD
6. Internal Samsung SATA SSD
7. ATA HDD1
8. ATA HDD2
9. USB HDD
10. PCI LAN

These entry labels are recorded verbatim. The operator confirms OpenBSD boots
from the internal Samsung SSD, but the current evidence does not map either
Linux Boot Manager entry to that installation. OpenBSD recovery without a
persistent boot-order change is confirmed; the precise return selection is
not identified.

The operator reports inspecting these settings without changing them. Whether
settings were changed during earlier Mangrove testing remains unknown. Lenovo
documents F12 as a one-time boot-device selection and F1 as ThinkPad Setup
entry ([Lenovo boot-menu instructions](https://support.lenovo.com/pk/en/solutions/ht104668),
[X250 BIOS reference](https://download.lenovo.com/pccbbs/mobiles_pdf/lenovo_thinkpad_x250_bios_security_white_paper.pdf)).

## Image identities and evidence boundaries

### Physical boot identity

| Image | Size | SHA-256 | Physical result |
| --- | ---: | --- | --- |
| `build/x250-gpt-capacity-trace/Mangrove/MangroveUSB.img` | 15,682,240,512 bytes | `a4da0b1a5ed2a38a707ebbceb07230b728b1b9c3b7bb4dfa30ac72654f3f8120` | Operator-confirmed physical boot to Shoot with xHCI SanDisk storage, GPT and MGFS root. This image predates the sustained PS/2 keyboard correction and is not the final keyboard-baseline image. |

The retained `build/x250-gpt-capacity-evidence/image-identity.json` verifies
that this image contains the recorded loader and Pith hashes and valid GPT
copies. Its accompanying KVM tests cover capacity-matched boot, late root
discovery, and rejection of a compact image whose backup GPT remains at the
wrong disk end. Those KVM results are not physical-hardware evidence.

### Compact keyboard build identity

`build/x250-ps2-sustained-normal/Mangrove/MangroveUSB.img` is retained as a
135,283,200-byte diagnostic-disabled build with SHA-256
`31af8cb19ec7689aba8bfe581cdc67848ee6e77808aab21dd465a76f7312c805`. Its
retained report verifies the embedded loader/kernel, GPT, MGFS, and PS/2-only,
USB-only, and combined KVM command-input tests. The operator later confirmed
sustained PS/2 input on physical X250, but the exact image hash used in that
physical test is not in the retained report. Confirm that identity before
listing a compact image as the physical keyboard recovery image.

The earlier compact image `build/x250-ps2-normal/Mangrove/MangroveUSB.img`
(SHA-256 `f91c2c48052e8aa70384d65429ffbd5954727353b431327b6cf06bfac5fdf877`)
booted to Shoot but had the reported short-input-then-stop failure. It is not a
known-good sustained-keyboard image.

Other ignored `build/x250-*` directories retain intermediate firmware
checkpoints, handoff and relocation builds, xHCI routing/port/root-discovery
investigations, compact-image/flasher tests, and keyboard host/KVM campaigns.
They include QEMU screenshots, serial output, build logs, image identities and
validation JSON. Their names and contents describe the test that produced
them; they must not be treated as physical observations unless an operator
result explicitly says so. `build/hardware-x250/usb-root.img` is a 64 MiB test
root image, not a captured physical disk. The tracked architecture/test docs
describe implementation contracts and KVM/host tests; no earlier tracked
X250 hardware-profile or recovery record was found.

Source-matched 1/2/4-vCPU scheduler/memory and 2/4-vCPU device qualification is
documented in [SMP architecture](../architecture/smp.md). It establishes
bounded KVM behavior only; it is not physical X250 SMP evidence.

## Read-only OpenBSD inventory

Run these commands in the existing OpenBSD installation. They query system
state only. Do not add `=value` arguments to `sysctl`, `wsconsctl`, or
`ifconfig`; those forms can change state. `doas` is used for device/kernel
reports that may be restricted to root. Return the output, redacting the
machine serial number, UUID, or network MAC addresses if desired.

```sh
uname -a
sysctl hw.vendor hw.product hw.version hw.model
sysctl hw.ncpu hw.ncpufound hw.ncpuonline hw.blockcpu hw.physmem hw.usermem hw.cpuspeed hw.perfpolicy hw.smt
doas dmesg | grep -E 'cpu[0-9]+:|SMT|blockcpu'
vmstat -s

# Optional: run only if dmidecode is already installed.
doas dmidecode -t bios -t system -t baseboard -t processor -t memory

doas dmesg
doas pcidump -v
doas usbdevs -vv
doas wsconsctl -a
doas ifconfig -a

sysctl hw.disknames hw.diskcount
doas mount
df -h
```

The reports establish:

- `uname`/`sysctl hw.*` identify the OpenBSD build, SMBIOS product/version
  strings, CPU counts and online CPUs, SMT blocking policy, memory size, and
  reported current CPU speed/performance policy. `dmesg` CPU lines help explain
  why discovered logical CPUs were not brought online. They do not establish
  Mangrove's CPU-online behavior. The recorded type/model comes from the
  supplied OpenBSD inventory.
- If already installed, `dmidecode` reads firmware-provided SMBIOS records for
  BIOS, product, board, processor, and memory modules. These records are not an
  independent hardware probe and may contain serial numbers; redact those
  before sharing. Do not install anything for this inventory.
- `dmesg` records firmware strings and the driver's attachment paths for
  CPUs/APICs, ACPI/timers, PCI devices, storage, USB, keyboards and pointing
  devices. `pcidump -v` supplies PCI bus/device/function and IDs. `usbdevs -vv`
  shows USB controller/device topology and descriptors.
- `wsconsctl -a` reports the active keyboard encoding and console/input
  configuration. `ifconfig -a` identifies available network interfaces and
  current link/address state; it does not transmit test traffic.
- `hw.disknames`, `mount`, and `df -h` identify disks and the current OpenBSD
  root device. OpenBSD `dmesg` supplies storage model strings. If a disklabel
  is needed, use only the display form `doas disklabel -h sdN` (substitute a
  disk name reported above); do not use `-E` or `-w`.

Run the following read-only time, ACPI and sensor queries:

```sh
sysctl kern.timecounter.hardware kern.timecounter.choice kern.timecounter.tick kern.clockrate
sysctl hw.sensors
date -u
doas acpidump
```

`kern.timecounter.*` and `dmesg` identify OpenBSD's selected timecounter and
reported ACPI timer/APIC devices; `date -u` reports the current wall clock;
`hw.sensors` reports sensors OpenBSD has attached. `acpidump` emits the
firmware ACPI tables for reference. These describe OpenBSD's view of the
machine, not Pith's timer selection or ACPI behavior. An absent OpenBSD sensor
does not prove the hardware has no sensor.

The operator inspected ThinkPad Setup read-only, recorded the displayed values
above, and exited without changing them. Whether any settings were changed in
earlier Mangrove testing remains unknown. If repeating the inventory, use F1
to enter Setup and Escape to leave without saving; menu labels vary by firmware
version.

The command syntax follows the OpenBSD [`sysctl(8)`](https://man.openbsd.org/sysctl),
[`pcidump(8)`](https://man.openbsd.org/pcidump),
[`usbdevs(8)`](https://man.openbsd.org/usbdevs),
[`wsconsctl(8)`](https://man.openbsd.org/wsconsctl), and
[`acpidump(8)`](https://man.openbsd.org/acpidump) manuals.

## X250 image and recovery notes

Normal USB images are compact (135,283,200 bytes). The Linux
[`flash-usb.sh`](usb-flashing.md) utility writes that prefix and relocates the
backup GPT to the destination's actual end, validates the resulting GPT and
partition contents, and requires a typed, device-specific confirmation. It
checks removable USB identity, capacity, sector geometry, mounts and host
system-disk relationships. It is Linux-specific; do not run it from OpenBSD.
Do not use plain `dd` to copy a compact image onto the larger SanDisk: the
backup GPT would remain at the image end and Pith correctly rejects that
layout. Do not flash the full-capacity image as part of routine development.

The generic physical-test preparation, boot, and recovery procedure is in
[physical test recovery](physical-test-recovery.md). On the Arch Linux
development host, after selecting and independently checking a compact image
and confirming the whole-device `/dev/disk/by-id/` name, the X250 command shape
is:

```sh
sudo ./scripts/flash-usb.sh \
  --image build/Mangrove/MangroveUSB.img \
  --device /dev/disk/by-id/usb-REPLACE_WITH_CONFIRMED_SAN_DISK_ID \
  --expect-model 'Cruzer Blade' \
  --mgfsck build/mgfsck
```

The placeholder is not a device selection. The operator must replace it with
the verified whole-disk identity and type the utility's device-specific
confirmation. The utility has not been run as part of this profile update. No
hash is claimed for the compact image used in the physical sustained-keyboard
test.

For a repeatable X250 test, retain the internal OpenBSD installation and its
firmware boot configuration. F12 Boot Device List is configured Enabled, and
the operator has confirmed that it opens and can boot Mangrove from the
SanDisk. USB HDD is ninth in the displayed persistent boot priority. OpenBSD
boots from the internal SSD, and the operator can return to it without
changing persistent boot order; the specific one-time entry used for that
return is not recorded. The firmware is set to Both with UEFI First, and
Secure Boot is Disabled. Do not change these settings for routine testing.

1. With the SanDisk test media disconnected, start the X250 and use the
   confirmed recovery path to reach the existing OpenBSD installation. Record
   whether this uses the unchanged boot order or a one-time F12 choice if the
   path matters to a particular test.
2. Use independently verified Mangrove test media. Do not flash media solely
   for this recovery-path check. If new media is needed later, use the Linux
   command above, keep the OpenBSD disk out of the target selection, and
   confirm the utility's device-specific target identity.
3. At startup, use the confirmed F12 menu (Fn+F12 if required) and select the
   SanDisk for this boot only. The operator has confirmed this selection boots
   Mangrove. If the menu or device entry becomes unavailable, stop; do not
   change persistent firmware settings as part of this procedure.
4. If Mangrove does not boot, power down, disconnect the SanDisk, and return
   to OpenBSD using the recovery path verified in step 1. Do not write to the
   internal disk. If the verified path no longer works, stop and report the
   firmware screen rather than attempting repair or changing settings.

The operator confirms steps 1 and 3 from normal physical use. These are
operator observations rather than a separately instrumented test. Whether
earlier Mangrove testing changed any firmware values remains unknown. If a
later authorized test changes a setting, record its old value first and
restore that exact value; do not load defaults or change Secure Boot/UEFI mode
as a generic recovery method.

## Nonblocking evidence gaps

- The compact image hash used for sustained physical keyboard input is
  unknown. This limits image attribution for that keyboard result; it is not a
  separate firmware/recovery check. The capacity-matched image above remains
  the only exact image identity tied to the recorded physical boot to Shoot.
- `hw.blockcpu` and per-CPU OpenBSD attachment lines remain unavailable. This
  is useful for explaining OpenBSD's 4-found/2-online result but is not needed
  to infer Mangrove's CPU support; Mangrove's online CPU set is measured
  independently.
- Other unrecorded details include the system-board revision, exact PCI
  function/revision for the network and storage devices, external connector
  mapping, keyboard layout, and the exact RTC/shutdown warning text. These are
  not prerequisites for the Task 1 recovery baseline.
