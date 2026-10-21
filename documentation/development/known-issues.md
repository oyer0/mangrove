# Known issues and qualification limits

This register contains current, unresolved issues supported by retained
evidence. Passing later runs do not close an intermittent issue by themselves.

## Open correctness issues

### Intermittent scheduler restored-stack guard panic

- **Symptom:** Pith fail-stops with `scheduler: restored stack outside kernel
  stack` while restoring a scheduler context.
- **Observed configurations:** Two-vCPU indexed-only runs and hybrid runs,
  including instrumented hybrid-384. The retained qualification notes record
  stack-guard failures during indexed-only MGFS verification and hybrid-384
  create/verify phases. Matched indexed-only,
  hybrid-256, uninstrumented hybrid-384, and later diagnostic hybrid-384
  campaigns also completed; intermittent passes do not establish a fix.
- **Reproduction conditions and evidence:** It has appeared in bounded
  two-vCPU QEMU TCG system workflows, particularly filesystem verification.
  The guard logs CPU/thread and stack-range context before fail-stop. See the
  retained Stage 4.7 reports under
  `build/stage47-qualification-20261020/` and the qualification summary in
  [memory architecture](../architecture/memory.md). The available evidence
  does not establish a deterministic trigger or root cause.
- **Impact:** Affected boots cannot complete their workload. This blocks a
  claim that the scheduler/system path is fully qualified; it is not evidence
  by itself of allocator corruption.
- **Closure criteria:** Reproduce and identify the root cause; add a focused
  regression test for the corrected invariant; then complete a source-matched,
  repeated one- and two-vCPU matrix for indexed-only and supported hybrid
  normal/deep configurations without the panic. Keep the stack-range guard
  enabled.

### Historical MGFS `volume-safe-create` child status 43

- **Status:** One earlier safe-create qualification reported child status 43;
  this remains an unexplained qualification incident, not a demonstrated
  filesystem defect. Current trusted-clock create/verify/read-only
  campaigns pass for indexed-only and hybrid-256, but do not explain or close
  the historical result. See [focused test evidence](tests.md).
- **Impact:** The historical safe-create result remains unresolved; current
  passing campaigns qualify their own source and configuration only.
- **Closure criteria:** Recover the exact failing run inputs and logs, reproduce
  the same safe-create failure, identify its cause, and add a focused regression
  test or evidence-based explanation showing the result was a harness/config
  failure rather than a product defect.

This status-43 report is distinct from the intentional wrong-content negative
control, which is expected to exit 43, and from a separate time-test run built
without `RTC_UTC_TRUST=1`, which is a test-configuration failure. Neither is
evidence that the historical safe-create result has been resolved.

### Native test child creation failure

The Stage 4.7 qualification notes record an indexed-only startup where the
Ring 3 test process was not created. In this allocator slice, the two-vCPU
deep-validation guest printed `SLAB_GUEST_PASS` and then reported
`Ring 3 process creation failed`; its runner timed out waiting for completion.
This is a separate symptom from the restored-stack panic, and no cause has
been established. It prevents that guest campaign from qualifying the later
native process phase. Closure requires capturing the failing process-creation
reason and completing the same source-matched guest phase successfully; do not
infer that allocator success in the slab test closes the later failure.

## Host-tooling qualification limitation

### LeakSanitizer initialization in the current environment

The current host environment has produced a LeakSanitizer fatal initialization
error (including the runtime's ptrace limitation). ASan/UBSan tests can be run
with `ASAN_OPTIONS=detect_leaks=0`; those runs do **not** qualify leak
detection. This is a tooling limitation, not a Mangrove kernel defect.
Closure requires a host environment where LeakSanitizer initializes and the
relevant sanitizer-backed fixtures complete with leak detection enabled.
