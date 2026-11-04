# Native HVC: installed, useful consoles unavailable

Native HVC core/driver autoload and two failed-open/unload cycles pass with clean teardown, no missed trace events and zero payload writes; default native mode is restored. All five console-open requests return raw RM status 6; useful console access is unavailable in the tested kernel/ROM/firmware combination. Guest visibility and console provisioning remain unresolved and deferred. Successful console open/read and retained-file behavior were not established on the phone; retained-file paths have host test coverage only. No guest command, VM restart or deliberate crash.

[Final acceptance](../validation/onyx-runtime-summary-20261104.md); [installed availability](../validation/onyx-runtime-summary-20261104.md); [exact firmware findings](../validation/onyx-runtime-summary-20261104.md). The native four-byte console-open packet matches this firmware. Static lookup analysis does not measure live RM pointers or distinguish an absent VM record, configuration or console object. The corrected kernel retains the original signed GKI modules and their public trust certificate.

---

Previous design and validation notes follow.

# Native Gunyah HVC port

The driver ports the complete Onyx non-printk HVC backend from Xiaomi revision
`10570812a4878b644e31b47c2f0bd052664cf86d`, native blob
`eb18ed8a795eabdc307852fc82d70947c67d8835`. Production patch
`0235-port-native-gunyah-hvc-backend-lifetime.patch` applies to that complete
native file. It preserves the five VM consoles, 1 KiB input/output FIFOs, native
RM console open/write/flush/close RPCs, and removal/recreation on VM restart.
`CONFIG_HVC_GUNYAH_CONSOLE` remains disabled as on native Onyx.

The common kernel must provide the reviewed `linux/hvc_backend.h` API and RM
console packet length guard. The module is not compatible with the original
GKI without those core repairs. Private backend allocation followed by atomic
group publication replaces pre-instantiation, so no failed initialization can
leave a console reservation or expose callbacks from a failed module. All
allocation and notifier-registration failures propagate to the caller.

Each VM has one FIFO spinlock, a session mutex for open/close/flush, and an RPC
mutex shared with its output worker. Atomic get/put callbacks bound size_t
requests to the FIFO capacity. Output scheduling occurs under the same spinlock
that closes admission before work cancellation. Session close drains accepted
output while the VM remains online and resets both FIFOs under their lock.
VM retirement disables admission, detaches the pointer from notifier polling,
drains native-core callbacks, explicitly closes any remaining RM session, and
cancels work before state can be reused. The explicit close also handles module
exit when the core cannot acquire the backend module owner.

A lifecycle mutex serializes complete VM removal and recreation. A separate
notification mutex protects published backend pointers and polling. Neither is
taken by HVC callbacks. Both native notifier chains are drained outside these
locks during shutdown and failed initialization. Native RM validates the
reassembled notification length before its existing unsized compatibility
bridge copies the console payload. The driver checks every VM mapping before
indexing. It ignores console input while a VM is retired or its console closed.
The wire format carries no generation number: bytes delayed until after a new
open using the same VMID cannot be distinguished from new output.

The optional `read_only=1` module parameter rejects all console payload writes,
including TTY echo before userspace can set raw/no-echo termios. It is immutable
after insertion (`0444`) and defaults to false, preserving native write behavior.
Live diagnostics use this mode for read-only open/read/close checks; successful
retained-descriptor I/O was not established. Host tests use mocked RPCs; no guest command injection is needed.

The production recipe includes the pinned import, patch, Kconfig/defconfig,
Bazel target and module catalog entry. The historical HVC deployment checkpoint
contained 388 platform modules; the current port contains 391. Full GKI/source
replay and import checks passed. Raw device receipts and local harness fixtures
remain outside the published portable CI inputs.

The whole-driver actual-C harness passes 94 ASan/UBSan assertions and rejects
four mutations. It covers all five allocation failures, both notifier-registration
failures, batch publication failure, initialization/exit, SIZE_MAX I/O lengths,
read-only rejection, open/close errors, work admission, late input while retired,
recreation failure, and explicit close when the module owner is going away.
Its deterministic host services do not establish real concurrency or firmware
behavior. The source hash and detailed receipts are retained locally; the public
[runtime summary](../validation/onyx-runtime-summary-20261104.md) records current
coverage. These 94 assertions are local host evidence, not a portable CI gate.
