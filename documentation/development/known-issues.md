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

### Historical MGFS `volume-safe-create` child status 43

An earlier safe-create campaign reported child status 43; later trusted-clock
indexed-only and hybrid-256 create/verify/read-only campaigns passed, but do
not explain the earlier result. The intentional wrong-content control also
exits 43 in a separate phase. The failed run's `RTC_UTC_TRUST` build setting
is not preserved, so it cannot be distinguished confidently from the
corrected test-configuration mismatch. It is not tracked as a confirmed
filesystem bug. See [focused test evidence](tests.md).

### Native test-child creation failure

Some two-vCPU deep-validation qualification attempts completed the slab guest
probe but then failed to create the Ring 3 test process or timed out before
collecting guest evidence. The cause is unknown and has not been shown to be a
kernel defect. Keep these as qualification failures; do not infer allocator
corruption or count the affected run as a pass. Broader scheduler and boot
qualification limits are summarized in the [memory architecture
record](../architecture/memory.md).

## Host-tooling qualification limitation

### LeakSanitizer initialization

LeakSanitizer has failed to initialize in the current host environment,
including a runtime ptrace limitation. ASan/UBSan runs with
`ASAN_OPTIONS=detect_leaks=0` do not qualify leak detection. This is a tooling
limitation, not a Mangrove kernel defect. Leak qualification requires a host
environment where LeakSanitizer initializes and the relevant tests complete
with leak detection enabled.
