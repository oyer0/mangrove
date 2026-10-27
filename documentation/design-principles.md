# Mangrove design principles

Mangrove is being developed as a coherent native operating system. This
document guides design and review; it does not describe a completed feature
set or promise that every current interface is stable. Subsystem documents and
source code describe implemented contracts. Future goals below require their
own designs, tests, and compatibility decisions before they become contracts.

## Correctness and failure behavior

Correctness, reliability, and data integrity take precedence over the speed
of feature delivery. An operation should have explicit preconditions,
ownership, and failure states. In particular, storage and concurrent code
must distinguish a confirmed result from an outcome that may have changed
persistent state. An uncertain outcome must not be reported as success or
silently retried. Where a guarantee cannot be provided, the interface and
documentation should state its limit plainly.

Tests should cover adverse conditions as well as successful use: allocation
failure, partial I/O, device removal, stale identities, concurrent operations,
and teardown. A passing host fixture establishes only the behavior it actually
executes; guest, hardware, and power-loss properties need separate evidence.

## Efficiency and implementation quality

Performance and resource efficiency are design goals alongside correctness.
Avoid unnecessary allocations, background work, duplicate scans, and layers of
abstraction that do not serve a demonstrated need. Prefer bounded work and
clear resource ownership, especially in the kernel and long-lived services.
Measure relevant workloads before claiming an optimization helps; include
memory use and failure-path cost when comparing designs.

Prefer implementations that a maintainer can reason about: named invariants,
defined lock order, explicit lifetimes, consistent conventions, and the least
complexity needed for the required behavior. Small changes often reduce review
risk, but a refactor or subsystem rewrite is appropriate when evidence shows
that it improves correctness, interoperability, efficiency, or maintainability
enough to justify its migration and test cost.

## One system, usable interfaces

Kernel mechanisms, native libraries, system services, and applications should
meet at defined interfaces rather than accumulate incompatible mechanisms for
individual programs. Put authority and shared invariants at the layer that
can enforce them; leave application choices at the application layer. For
example, the existing VFS conditional replacement mechanism checks file
identity and generation. An editor determines how to present a conflict and
what choices to offer the user.

As public interfaces mature, first-party programs should use the same stable,
documented interfaces offered to third-party developers under equivalent
authorization rules. Application-specific private privilege should not be a
substitute for a documented general interface when the capability is intended
for third-party use, and applications should not depend on undocumented kernel
or service internals. Architectural consistency does not require every
application to have identical behavior or appearance.

## Native environment and standards

Mangrove's own libc, startup code, standard headers, and system interfaces are
the normal development environment for native applications. Native target
binaries should not depend on another operating system's runtime or libraries
unless that dependency is explicitly approved. Applications should not need
Linux-specific facilities or conventions to target Mangrove.

An established standard is a contract, not a requirement to import someone
else's implementation. When Mangrove exposes a standard interface, its
behavior, types, and public namespace should follow that standard. Mangrove
can implement specifications such as ISO C17, ELF, and UEFI independently.
Incomplete coverage must be documented; a familiar standard name must not
silently carry incompatible behavior. Standard C headers should contain their
required declarations and preserve standard namespace rules. Mangrove-specific
capabilities belong in documented native interfaces that include the types
and declarations needed to use them.

The native libc and SDK should be available to first-party and third-party
developers under the same documented contracts and authorization rules.
Applications may use them, provide their own runtime where appropriate, or
use documented native system interfaces directly. Mangrove should not require
a particular toolkit, runtime, or application architecture.

GCC, GNU binutils, and host-side build and test tools are development tools;
using them does not imply importing their libraries into target binaries.
Compiler-generated runtime helpers must be identified and supplied through
explicitly approved implementations. Standards support can grow in stages,
but implemented behavior should be verified with tests that exercise the
production code. This principle does not claim that Mangrove currently
provides a complete ISO C17 implementation.

## International text and input

International text support is a continuing system goal, not a Latin-only
extension. Preserve valid text even when the current input method, renderer,
font, or application cannot fully enter or present it. Keep UTF-8 storage,
grapheme editing boundaries, word segmentation, display-cell layout, shaping,
bidirectional ordering, and input composition as distinct contracts. Do not
use display limitations as a reason to corrupt, normalize, or silently discard
text. Support levels and unsupported presentation or input behavior must be
documented and tested at the layer that owns them.

## Keyboard modifier conventions

Unmodified keys perform their ordinary actions. Alt broadens the scope or
granularity of an action while preserving its purpose, such as moving one
character with Left and one word with Alt+Left. Ctrl invokes a distinct
command or function, such as typing `L` versus clearing the terminal with
Ctrl+L. These are guidelines, not rigid rules; preserve established bindings
unless a specific inconsistency is reviewed separately.

## User control and extensibility

Provide approachable defaults and clear feedback while respecting users'
competence. Normal computer functionality and meaningful controls should
remain available; simplicity should not depend on hiding state, forcing one
workflow, or silently choosing on the user's behalf.

Customization and extension should be broad within explicit, stable
contracts. Those contracts should protect system invariants, usability,
accessibility, and predictable behavior. A future native theming system should
allow extensive customization through supported theme surfaces, without
requiring internal widget overrides that can break application layouts. This
is a goal for a future design, not a claim that such a theming API exists or
that arbitrary third-party rendering can be prohibited.

## Documentation and decisions

Architecture, public APIs, errors, lock ordering, operational guides, and
test boundaries are engineering artifacts. Update them as implementations
change, and identify what remains untested or unsupported. Documentation
should describe observed behavior and approved plans separately.

When designs compete, evaluate them in this order of questions:

1. **Correctness:** What invariants and failure guarantees does each design
   establish, and what remains uncertain?
2. **Simplicity and maintainability:** Can ownership, behavior, and future
   changes be understood without unnecessary mechanisms?
3. **Interoperability:** Does the design fit existing boundaries and provide
   a usable contract for other components and third-party developers?
4. **Efficiency:** What do measurements or bounded-cost analysis say about
   time, memory, I/O, and background work?
5. **User control:** Are choices and consequences visible, with meaningful
   control left to the user?

These questions guide judgment rather than impose a fixed scoring formula.
Record material trade-offs and revisit decisions when implementation or
measurement provides better evidence.
