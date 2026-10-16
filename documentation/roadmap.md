# Development status and next acceptance gates

## Current position

**Stage 2J is complete. Stage 2K guest filesystem integration is complete.
Stage 3 is active; Stage 3B runtime foundations are implemented and Stage 3C
formatting/streams are next.** This status covers the supported, bounded
runtime contracts below, not a finished operating system or universal media
compatibility.

The guest integration acceptance boundary is native x86-64 UEFI boot and Ring 3
processes on Q35/OVMF, one vCPU, xHCI USB storage and QEMU TCG. Public syscalls,
ordinary credentials, PASS, services and production VFS/filesystem/block code
remain in the path. MGFS, FAT32 and exFAT have qualified persistence and
secondary-volume lifecycles, safe-file/application workflows and deterministic
native stress workloads. Independent stopped-media checking is required.

See [integration qualification](development/integration-qualification.md) for
coverage, evidence boundaries and reproducible entry points. A passing result
never authorizes repair or abandonment of an unresolved runtime recovery owner.

## Runtime implementation direction

The [runtime architecture](architecture/c-runtime.md) records current C, time
and Unicode contracts and demonstrated gaps. The
[runtime plan](development/runtime-plan.md) tracks the implemented core ABI and
the remaining streams, time, shared text and length-aware native interfaces.
This is a bounded baseline plan, not a claim of complete hosted C17 conformance.

## Explicitly deferred work

| Destination | Remaining work and acceptance boundary |
| --- | --- |
| Stage 3: libc and native Unicode interfaces | Develop the broader libc contract and length-aware pathname/enumeration interfaces. The current 255-byte UTF-8 native component limit is an ABI restriction, not an on-disk format limit; checked errors must remain visible. |
| Later filesystem engineering | Increase demonstrated full-volume/proof scalability and safe-content/mapped-replacement bounds; reduce FAT32 metadata-I/O amplification and exFAT retained transaction memory without weakening ownership or uncertainty containment. Existing limits reject unsupported work explicitly. |
| Stage 6: SMP | Qualify shared lifetimes, locking, publication and storage concurrency with multiple guest CPUs. Controlled host concurrency and one-vCPU guests do not establish SMP correctness. |
| Stage 7: physical hardware | Qualify actual USB/controller behavior, DMA teardown, flush semantics, device disappearance and hardware-specific time trust. TCG is not hardware evidence. |
| Stage 9: persistent recovery | Design crash consistency, persistent recovery evidence and post-reboot repair. Current pins and private owners provide runtime exclusion only. |
| Mandatory pre-desktop acceptance | Establish system-wide shutdown/quiescence, reliable resource/leak evidence, Unicode completeness and acceptable memory/I/O/latency under realistic workloads. Reboot, power-loss and hardware acceptance need their own evidence. |

The unified comprehensive `make test-all` command remains unqualified end to
end. Its constituent behaviors have separate qualification; exercising the
entire expensive command is useful infrastructure acceptance, not a missing
filesystem capability. Manual network/boot diagnostics, macOS guest campaigns,
physical devices and destructive media tools are not implicitly qualified by
MTS host or standard guest PASS.
