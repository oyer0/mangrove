# Known issues and qualification limits

This page summarizes current issues. GitHub Issues are the primary tracker for
confirmed unresolved bugs; host-tooling limitations and unclassified test
incidents are recorded separately.

## Confirmed unresolved correctness issue

### Intermittent scheduler restored-stack guard panic

Pith has intermittently fail-stopped with `scheduler: restored stack outside
kernel stack` during two-vCPU QEMU TCG workflows. It has occurred in both
indexed-only and hybrid configurations, including MGFS verification and
instrumented hybrid-384 create/verify runs. Comparable campaigns have also
passed. The root cause is unknown; occurrence with hybrid enabled does not
demonstrate an allocator defect. Affected runs cannot complete, leaving
scheduler/system qualification incomplete. Keep the stack guard enabled.

Track reproduction, evidence, unknowns, and closure criteria in
[GitHub issue #1](https://github.com/oyer0/mangrove/issues/1). The qualification
summary is in the [memory architecture record](../architecture/memory.md), and
the guard implementation is in [`scheduler.c`](../../kernel/src/scheduler.c).

## Unresolved qualification incidents, not confirmed defects

### Intermittent PID 1 Ring 3 process-creation failure

Some native boots have stopped during PID 1 construction with
`[FAIL] Ring 3 process creation failed`, before ELF loading. Historical
reports include indexed-only and hybrid builds, but do not identify the
failing prerequisite or provide a reliable reproducer. The boot path now logs
the failed construction stage and status. Later indexed-only one-vCPU and
diagnostic hybrid-256 two-vCPU captures identify root working-directory
lookup returning `VFS_ERR_DEVICE_GONE` (`-11`), preceded by USB transfer/BOT
errors. This identifies those failures' construction stage, not the cause of
device loss or every historical failure. Other runs passed.
No shared cause with the scheduler panic is established. Track investigation
and closure evidence in [GitHub issue #2](https://github.com/oyer0/mangrove/issues/2),
separately from [scheduler issue #1](https://github.com/oyer0/mangrove/issues/1).

### Historical MGFS `volume-safe-create` child status 43

An earlier safe-create campaign reported child status 43; later trusted-clock
indexed-only and hybrid-256 create/verify/read-only campaigns passed, but do
not explain the earlier result. The intentional wrong-content control also
exits 43 in a separate phase. The failed run's `RTC_UTC_TRUST` build setting
is not preserved, so it cannot be distinguished confidently from the
corrected test-configuration mismatch. It is not tracked as a confirmed
filesystem bug. See [focused test evidence](tests.md).

### Historical combined time-and-network guest exit 43

One combined guest attempt exited 43 without a valid time/network phase
transcript. Its precise failing subcheck is unknown, so this remains an
unclassified qualification incident rather than a confirmed network or time
defect. The separate network RX and safe-file evidence is summarized in
[focused test evidence](tests.md).

## Host-tooling qualification limitation

### LeakSanitizer initialization

LeakSanitizer has failed to initialize in the current host environment,
including a runtime ptrace limitation. ASan/UBSan runs with
`ASAN_OPTIONS=detect_leaks=0` do not qualify leak detection. This is a tooling
limitation, not a Mangrove kernel defect. Leak qualification requires a host
environment where LeakSanitizer initializes and the relevant tests complete
with leak detection enabled.
