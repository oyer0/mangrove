# USB device support

Pith currently supports native xHCI enumeration, USB boot-protocol HID
keyboards, USB Mass Storage Bulk-Only Transport, and hubs needed to reach
devices. Enumeration and class-driver binding are separate outcomes: a device
can be fully enumerated and retained in the device snapshot without a matching
class driver.

## Class-driver binding

The xHCI descriptor probes use `XHCI_ERR_NOT_SUPPORTED` only when valid
configuration descriptors were read and no supported interface matched. Such
a device completes enumeration as `READY`, is exposed as a generic `USB
device` with driver `unsupported`, and does not produce a normal error merely
because Mangrove has no class driver. This outcome does not imply that device
functions are usable.

Descriptor transfer/controller/resource errors are propagated as setup
failures. Malformed descriptor chains and recognized supported interfaces
missing required endpoints return a transaction error. A matched class whose
endpoint or class initialization fails also remains a failure. Neither the
port number nor VID:PID changes this policy. Runtime enumeration failures
remain visible through the port failure result and error log; `XHCI_DEBUG=1`
and `X250_BOOT_TRACE=1` provide opt-in detail about controller work and
binding probes. Normal production builds do not include the X250 overlay.

## Identified but unsupported X250 peripherals

OpenBSD 8.0 identifies these integrated peripherals in the X250 USB topology:

| USB port | Device | VID:PID | Current Mangrove policy |
| ---: | --- | --- | --- |
| 6 | Validity Sensors VFS5011 fingerprint reader | `138a:0017` | Unsupported; no fingerprint driver |
| 7 | Intel Bluetooth | `8087:0a2a` | Unsupported; no Bluetooth driver |
| 8 | SunplusIT integrated camera | `5986:0366` | Unsupported; no camera driver |

Fingerprint, Bluetooth, and webcam functionality are postponed and are not
required for the current Stage 7 baseline. These identities strongly correlate
with prior Mangrove reports of runtime setup failures on ports 7 and 8, but do
not identify the failing setup phase. The observed results were return code 6
(`CONTROLLER_BAD`) on port 7 and return code 3 (`INVALID_PARAM`) on port 8.
Those are low-level setup failures, not successful enumeration followed by an
unsupported-class result. They remain genuine failures requiring further
investigation; this policy change does not suppress, reinterpret, or retry
them. The available evidence does not establish whether either peripheral
reached descriptor-based driver binding.

The device identities above are OpenBSD observations; they do not establish
Mangrove's enumeration result or driver support.

## Relevant interfaces and tests

- Class descriptor probes: `kernel/src/usb/xhci_descriptor.c`.
- Enumeration and class attachment: `kernel/src/usb/xhci.c`.
- Runtime port result and failure reporting: `kernel/src/usb/xhci_port.c`.
- Generic USB device snapshot: `kernel/src/device.c`.
- Focused host regressions: `tests/storage/xhci_descriptor_probe_test.py`,
  `tests/storage/xhci_usb_support_test.py`, and
  `tests/storage/device_snapshot_test.c`.
- Boot storage discovery requirements: [boot flow](boot.md#native-usb-root-discovery).
