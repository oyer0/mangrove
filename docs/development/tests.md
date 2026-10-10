# Focused test coverage

`gmake test` runs timekeeping, terminal, Reed, MGFS migration, VMM, heap,
history, synchronization, storage, development-image guard, and password tests.

| Target | Production code exercised | Test boundary |
| --- | --- | --- |
| `test-vmm` | User mapping/path allocation functions | Physical tables, mutexes and shootdowns are fixtures; no SMP integration. |
| `test-heap` | Allocation/free and local validation | Heap backing is host memory; growth is a fixture. |
| `test-history` | Shoot history/ring logic; VFS and MGFS merge/snapshot; MGFS write/truncate | Separate state mocks and fault-injected block fixtures; not crash recovery. |
| `test-sync` | Scheduler handoff publication/selection, mutex wake/ownership, recursive block gate | Deterministic interleavings and four pthread workers; scheduler parking and IRQs are fixtures. No guest scheduling claim. |
| `test-storage` | Block retirement, GPT backing cleanup, mutation admission; AHCI completion/recovery, cache/read-ahead and USB batching | Pthread/IRQ fixtures, simulated MMIO, timing and DMA, mock disks/USB commands and removal. Not hardware integration. |
| `test-terminal` | UTF-8 decoder; terminal scroll/render/overlay/input positioning | In-memory framebuffer and fixture font/locks; not graphical QEMU. |
| `test-dev-tools` | Development-image update script guard | Disposable images and missing/busy/idle tool fixtures. |
| `test-password` | SHA-256, HMAC, PBKDF2 and authentication | Host compiler/entropy fixture; comparison with Python references. |

`test-launch` remains a compatibility alias for VMM, heap and history tests.
The oversized harness has been split into subsystem files. Temporary C programs
are extracted from production definitions; fixtures duplicate only the required
surrounding ABI. These tests do not establish whole-kernel integration or ABI
compatibility by themselves. Build and guest tests are still required.

For guest verification, use a disposable image (`STATE_DIR` outside `.mangrove`)
and QEMU `-snapshot`. Exercise `history` options, nested session saves, external
history replacement followed by `-n`, and completion/editing on a command that
wraps over several rows. Never use the user's persistent image for destructive
history or fault-injection fixtures.
