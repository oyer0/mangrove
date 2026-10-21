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

### Intermittent PID 1 Ring 3 construction failure

Some boot attempts stop at `[FAIL] Ring 3 process creation failed` after the
root filesystem is reported ready. This message comes from PID 1 construction
in `start_pid1()`, before ELF loading; it is not a failure to create the
runner's later userspace test child. Historical captures do not identify which
`process_create()` prerequisite failed. The boot path now reports that stage
and any root-CWD VFS status if the failure recurs. Recent source-matched
indexed-only two-vCPU MGFS and hybrid-256 one-/two-vCPU guest runs passed, but
those passes do not explain or resolve the earlier failures. No root cause has
been established, so retain these as unresolved qualification incidents and
do not infer allocator corruption.

## Host-tooling qualification limitation

### LeakSanitizer initialization

LeakSanitizer has failed to initialize in the current host environment,
including a runtime ptrace limitation. ASan/UBSan runs with
`ASAN_OPTIONS=detect_leaks=0` do not qualify leak detection. This is a tooling
limitation, not a Mangrove kernel defect. Leak qualification requires a host
environment where LeakSanitizer initializes and the relevant tests complete
with leak detection enabled.
