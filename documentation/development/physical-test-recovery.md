# Physical boot-test preparation and recovery

This procedure applies to Mangrove physical boot tests across hardware targets.
Machine-specific firmware menus, boot keys, recovery operating systems, and
media tools belong in each hardware profile. A QEMU result does not substitute
for a physical test.

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
