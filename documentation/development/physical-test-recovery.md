# Physical boot-test preparation and recovery

This procedure applies to Mangrove physical boot tests across hardware targets.
Machine-specific firmware menus, boot keys, recovery operating systems, and
media tools belong in each hardware profile. A QEMU result does not substitute
for a physical test.

## Physical run report and evidence

Use `tools/physical_qualification.py` to create and validate one JSON report per
physical run. The tool only reads the selected repository/build/evidence files
and writes a new report file. It does not inspect block devices, invoke the USB
flasher, boot a guest, or collect a physical result. Its output path must be
new; put reports under ignored `build/` storage or outside the repository.
Unignored in-repository paths are refused so repeated updates do not alter the
source identity being recorded. Each `update` writes another new report
revision rather than replacing the previous record.

The report records the hardware-profile identifier and document hash, a
firmware-settings snapshot and hash, Git revision plus tracked/untracked
working-tree fingerprints, loader/kernel/image sizes and SHA-256 values,
non-unique boot-medium description, build/diagnostic options, cold/warm/unknown
boot classification, expected actions, required stages, stage observations,
result-state history, and bounded evidence references. Use `UNKNOWN` when the
operator did not establish whether a run was cold or warm; do not infer it from
the boot path or elapsed time. `update --boot-classification` records a
corrected classification in a new immutable report revision. Build artifact hashes
identify those files independently; they do not by themselves prove that the
loader and kernel embedded in the image match the separately hashed files.
That relation remains `NOT TESTED` until an independent image-identity report
is attached and explicitly recorded. The source fingerprint is the repository
state at report creation; it does not prove which source tree a prior build
used. Attach existing build/image identity output for that provenance.

Before each run, select a new report directory and build directory. For
example, on a machine whose profile is `documentation/development/x250-hardware-profile.md`:

```sh
RUN=build/physical-tests/x250-cold-01
BUILD_DIR=build/physical-build/x250-cold-01
mkdir -p "$RUN/evidence"
make -j4 BUILD_DIR="$BUILD_DIR" usb-image mgfsck
```

Prepare a firmware snapshot JSON file under the report directory. Include only
settings relevant to the test; use `"unknown"` for values not observed. Do not
include serial numbers, MAC addresses, network identifiers, credentials, or
other unique personal/device identifiers.

Create it once; noclobber protects an existing snapshot from accidental
replacement:

```sh
(
  set -o noclobber
  cat > "$RUN/firmware.json" <<'JSON'
{
  "source": "operator inspection of firmware setup",
  "values": {
    "bios_version": "recorded value",
    "secure_boot": "recorded value",
    "boot_mode": "recorded value",
    "usb_boot_support": "unknown"
  }
}
JSON
)
```

Create the manifest before booting. The command below captures build/source
identity, but leaves every physical stage and the overall result as
`NOT TESTED`:

```sh
python3 tools/physical_qualification.py create \
  --profile-id ThinkPad-X250-20CLS1EY00 \
  --profile documentation/development/x250-hardware-profile.md \
  --firmware-snapshot "$RUN/firmware.json" \
  --loader "$BUILD_DIR/EFI/BOOT/BOOTX64.EFI" \
  --kernel "$BUILD_DIR/Mangrove/pith.elf" \
  --image "$BUILD_DIR/Mangrove/MangroveUSB.img" \
  --medium-kind USB --medium-model 'SanDisk Cruzer Blade' \
  --medium-capacity 15682560512 --medium-vid-pid 0781:5567 \
  --boot-classification COLD \
  --build-option X250_BOOT_TRACE=0 \
  --expected-action 'Reach Shoot and execute the selected commands' \
  --required-stage uefi_loader \
  --required-stage kernel_handoff \
  --required-stage kernel_initialization \
  --required-stage device_discovery \
  --required-stage root_mount \
  --required-stage shoot_input \
  --required-stage test_workload \
  --required-stage shutdown_recovery \
  --deferred-device 'Fingerprint, Bluetooth, and camera class functions are outside this test' \
  --output "$RUN/report-00.json"
```

Use a profile-specific identifier, firmware snapshot, medium model/capacity,
diagnostic options, expected actions, and required stages for each target. Omit
the X250-specific trace option for other builds; record the actual options used.
Choose only stages exercised by the test, and include `shutdown_recovery` as a
required stage only when that recovery path is part of the test. Deferred or
unsupported devices can be listed for context; their presence does not fail a
test unless the test explicitly requires them.

### Prepare and verify removable media

Select the whole removable device yourself, verify its identity, and follow the
existing [compact USB flashing procedure](usb-flashing.md). Do not use `dd` or
another writer. To retain the flasher's output as a bounded host-side
transcript, make the report evidence directory first and capture the command's
output while keeping the flasher's explicit confirmation:

```sh
set -o pipefail
FLASH_LOG=$(mktemp "$RUN/evidence/flash-usb.XXXXXX")
sudo ./scripts/flash-usb.sh \
  --image "$BUILD_DIR/Mangrove/MangroveUSB.img" \
  --device /dev/disk/by-id/REPLACE_WITH_OPERATOR_SELECTED_WHOLE_USB_DEVICE \
  --mgfsck "$BUILD_DIR/mgfsck" \
  2>&1 | sed -E \
    -e 's/^Image: .*/Image: <source image>/' \
    -e 's/^Destination: .*/Destination: <selected removable USB; unique identifiers redacted>/' \
  | tee "$FLASH_LOG"
```

The placeholder is not a device-selection mechanism: replace it only after
independently selecting and checking the whole removable target. The utility
requires its device-specific confirmation and validates the write/readback.
The filter prevents the transcript from retaining the device path, model, or
serial printed in the safety summary; keep the terminal output visible during
the operation. It does not alter internal storage. For previously verified
fallback media, reference its prior verification report and identify the
fallback medium separately; do not mark the newly built image as present on it
without evidence.

Record successful host-side media verification in a new report revision. The
evidence source is `AUTOMATED` because the referenced transcript
comes from the flasher; the command does not decide whether the physical boot
will pass:

```sh
python3 tools/physical_qualification.py update \
  --report "$RUN/report-00.json" --output "$RUN/report-01.json" \
  --medium-verification VERIFIED \
  --evidence-root "$RUN/evidence" \
  --evidence "transcript=$(basename "$FLASH_LOG")" \
  --evidence-source AUTOMATED \
  --evidence-description 'flash-usb.sh write and readback transcript'
```

If flashing or its verification fails, record `--medium-verification FAILED`
and retain the failure transcript. Do not proceed to boot on a failed or
unverified write. Use a distinct output revision, for example:

```sh
python3 tools/physical_qualification.py update \
  --report "$RUN/report-00.json" --output "$RUN/report-01-failed-flash.json" \
  --medium-verification FAILED \
  --evidence-root "$RUN/evidence" \
  --evidence "transcript=$(basename "$FLASH_LOG")" \
  --evidence-source AUTOMATED \
  --evidence-description 'flash-usb.sh failed write or readback transcript'
```

Reports whose boot medium is USB cannot reach physical
`PASS` without an automated flash/readback evidence reference.

### Record physical observations

After each action, copy the preceding report to a new path with `update`.
Record one stage result and an observation per invocation. Start the overall
result with `INCOMPLETE`; record only what was actually observed. Include
`--evidence-root "$RUN/evidence"` on every update or validation after evidence
has been attached. Example:

```sh
python3 tools/physical_qualification.py update \
  --report "$RUN/report-01.json" --output "$RUN/report-02.json" \
  --evidence-root "$RUN/evidence" \
  --status INCOMPLETE --stage uefi_loader --stage-status PASS \
  --observation 'Firmware selected the named USB entry and displayed the Mangrove loader'
```

Use these stage names to identify the first failure and what followed:

| Stage | Boundary recorded |
| --- | --- |
| `uefi_loader` | Firmware selection and loader execution |
| `kernel_handoff` | Loader-to-Pith transfer |
| `kernel_initialization` | Pith initialization before device discovery |
| `device_discovery` | Controller, storage, and other required-device discovery |
| `root_mount` | Required Mangrove root selection and MGFS mount |
| `shoot_input` | Shoot startup and required keyboard/input action |
| `test_workload` | Explicit command or operation and its result |
| `shutdown_recovery` | Requested shutdown/reboot or return to the recovery system |

Normal builds do not persist a general physical-boot trace. When persistent
diagnostics are needed, build with the opt-in `X250_BOOT_TRACE=1` option. Once
MGFS is mounted, that build stores its captured kernel log and trace under
`/sys/logs/boot.log`, preserving the preceding trace-enabled run as
`/sys/logs/boot.previous.log`. The files contain bounded completion metadata
and CRCs. Footer version 2 also stores monotonic timing aggregates for block
and VFS lock acquisition, MGFS mutation phases, and recovery-pin handoff.
Acquisition counts include immediate lock acquisitions and do not mean every
attempt blocked. These categories may be nested; compare each with the enclosing
preparation or payload duration rather than summing them. A valid footer proves
the final completion record was written; the duration of that footer write
cannot be included in itself. The raw trace is
captured before the payload write; its footer and extracted summary include
the later payload timing and final save status. Kernel events after this
boundary are not in that record. If logging failed, the trace overflowed, or
boot ended before persistence completed, host extraction reports
`INCOMPLETE`. A failure before the root MGFS mount cannot be recovered from
this mechanism. The exact build, two-boot KVM qualification,
and read-only physical-media extraction procedure are documented in the
[build and image guide](build-and-images.md#opt-in-boot-diagnostics).

After returning to the host, extract records from the detached whole USB
device to a new location under `$RUN/evidence/`. The extractor verifies both
GPT copies, runs `mgfsck` on a copied root partition, and writes the raw
records, parsed snapshot/trace, and extraction status report. Capture the
extraction directory's `extraction.json`, `mgfsck.txt`, and relevant trace
files as `AUTOMATED` evidence. Read and redact logs before attaching them; do
not include unique device or personal identifiers. Only the latest two trace
runs are kept, so extract them before a third diagnostic boot if both are
needed.

If no persistent diagnostic record exists or the image was built without the
trace option, photograph the full screen or write a concise operator note
under `$RUN/evidence/`. Do not claim that QEMU output, a KVM report, or the
absence of a panic is a physical observation. Attach host reports as
`AUTOMATED` and screen photographs/operator notes as `OPERATOR`. The tool
records each referenced file's relative path, byte count, and SHA-256. Each
evidence file is limited to 32 MiB, at most 64 references and 128 MiB total.
It does not copy evidence files or inspect their contents for technical
correctness. Keep referenced files inside `$RUN/evidence/`, using fresh
destination names; the tool will not copy or overwrite them.

If an independent host image-identity report establishes that the loader and
Pith hashes above match the components embedded in the selected image, attach
it and record that result. Otherwise leave the field as `NOT TESTED`:

```sh
python3 tools/physical_qualification.py update \
  --report "$RUN/report-02.json" --output "$RUN/report-03.json" \
  --evidence-root "$RUN/evidence" \
  --embedded-match VERIFIED \
  --evidence host_report=image-identity.json \
  --evidence-source AUTOMATED \
  --evidence-description 'Independent embedded image identity report'
```

The validator checks that the report is referenced and hashed; it does not
interpret the host report. Set `VERIFIED` only after reviewing its contents.

Update a stage with its actual observation; use `INCOMPLETE` where it was
attempted but not established, and `FAIL` where an observed failure occurred.
First record a failed stage while the overall run is still incomplete:

```sh
python3 tools/physical_qualification.py update \
  --report "$RUN/report-05.json" --output "$RUN/report-06.json" \
  --evidence-root "$RUN/evidence" \
  --status INCOMPLETE --stage device_discovery --stage-status FAIL \
  --observation 'No required boot-storage device was discovered'
```

For a terminal failure, record the failed stage, operator confirmation, and
operator-supplied evidence in the next revision:

```sh
python3 tools/physical_qualification.py update \
  --report "$RUN/report-06.json" --output "$RUN/report-07.json" \
  --status FAIL --failure-stage device_discovery --confirm-physical \
  --evidence-root "$RUN/evidence" \
  --evidence photo=failed-screen.jpg \
  --evidence-description 'Physical screen at the first failed discovery stage'
```

For a terminal `PASS`, every required stage must be `PASS` and have an explicit
operator observation. USB write/readback must be verified when USB is the boot
source, and an operator must provide at least one evidence reference and
explicitly set `--confirm-physical`. A terminal result cannot be changed;
start a new report for a new physical run.
`NOT TESTED` means there is no physical boot result. `INCOMPLETE` means work
was attempted without establishing all requested results. A `FAIL` must name
the matching failed stage. The report validator checks the state transitions,
stage requirements, local artifact/evidence hashes, and references. It does
not independently judge the truth of operator observations or the semantics
of an attached KVM/flasher report.

After all required stages have been recorded, add operator evidence and finish
a successful run with a new report revision:

```sh
python3 tools/physical_qualification.py update \
  --report "$RUN/report-all-required-stages.json" --output "$RUN/report-success.json" \
  --status PASS --confirm-physical \
  --evidence-root "$RUN/evidence" \
  --evidence photo=shoot-workload.jpg \
  --evidence-description 'Shoot prompt and requested command results on the physical target'
```

Validate the final revision against the same repository/build files and
evidence directory:

```sh
python3 tools/physical_qualification.py validate \
  --report "$RUN/report-success.json" \
  --repo . --evidence-root "$RUN/evidence" --require-pass
```

For an intentionally failed or incomplete run, omit `--require-pass`; a
structurally valid `FAIL` or `INCOMPLETE` report is not a successful hardware
qualification. `--metadata-only` skips local source/build/evidence byte
verification and is suitable only for checking report shape after artifacts
have been moved. It does not validate the referenced bytes and must not be
reported as full evidence validation.

## Before a test

1. Confirm that the machine's existing recovery operating system boots with
   test media removed. Record the boot path and leave its internal storage and
   firmware configuration unchanged.
2. Select a Mangrove image whose build identity and intended test are known.
   Record its path, byte size, SHA-256, build options, and retained
   verification report. Do not identify a physical result with another image
   merely because the build directory names are similar.
3. Prepare only explicitly designated removable test media using the
   repository's supported host-side utility. Read its admission, confirmation,
   write, and verification contract. Never use the recovery system disk as a
   test target.
4. Independently verify the prepared medium with the documented layout and
   payload checks. Keep the recovery operating system on separate internal or
   recovery media. Record which checks actually ran; a source-image hash alone
   does not prove what was written to a device.
5. Use the machine's documented one-time boot selection when available. A
   setup option marked enabled does not prove that its key/menu works or that
   the expected entries appear; qualify the selection path for the specific
   target. Do not change persistent boot order, security settings, or firmware
   defaults as a convenience. If one-time selection is unavailable, stop and
   review a machine-specific plan before changing firmware configuration.

## Run and record

Record the physical machine profile, image identity, firmware path used,
attached test devices, workload, and observable result. Distinguish physical
observations from host tests and emulator results. For interactive tests,
record the command or operation that completed, not only that a prompt was
displayed. Keep failure evidence and identify the build options that produced
diagnostic output.

Do not run destructive storage, firmware-update, or power-loss tests unless
they have a separately reviewed scope and explicitly designated disposable
targets. A successful shutdown and post-run readback do not establish
power-loss durability.

## Recover after a failed boot

1. Stop the test and power the machine down using its documented controls.
2. Remove the Mangrove test medium. Select the previously verified recovery
   operating system using the machine's documented one-time boot path, or use
   the unchanged firmware boot order if it selects that system.
3. Do not repair, repartition, reformat, or reinstall the internal recovery
   disk as part of ordinary boot recovery. If the known recovery path does not
   work, stop and report the observed firmware screen and media state before
   attempting further changes.
4. Diagnose the retained test logs and image identity from the recovery
   environment. Prepare replacement test media only after rechecking the
   source image and failure evidence.

For USB images, use the project's [compact USB flashing procedure](usb-flashing.md)
and the target-specific hardware profile for boot-menu and recovery details.
