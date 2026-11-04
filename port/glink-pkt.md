# Native GLINK packet devices

`glink_pkt.ko` restores the native `qcom,glinkpkt` character-device provider.
Onyx already exposes the firmware `bt_cp_ctrl` RPMSG channel and declares its
GLINK packet child and ROM device ownership. The missing provider prevents the
vendor native Xpan manager from opening `/dev/bt_cp_ctrl` before its sustained
codec DMA client. Restoring the node is a concrete kernel requirement; full
Bluetooth playback still needs a device retest.

The port preserves channel names, ioctls, transport signals, automatic native
intent allocation, real RX acknowledgements and firmware data. Full packet pages
map directly with read-only special PFN PTEs. Only partial boundary pages are
copied into zeroed allocations, so adjacent firmware bytes are never exposed.
IO/PFNMAP VMAs exclude writable/executable upgrades, fork copying, remapping and
GUP pins. Pending mappings retain their MM and exact packet identity. DONE,
partial unmap, process exit and disconnect revoke the owned range before any
firmware buffer can be returned. Result-copy faults revoke and requeue the real
packet without acknowledging it; an internal cookie protects a newer mapping
at the same address from an older rollback.

RX metadata occupies skb control storage without replacing its owned head.
Ordinary reads retain their packet on copy faults, serialize fragments and take
a temporary copy before faulting userspace access. A reset generation prevents
stale read completion from consuming a replacement packet. Zero-length writes retain the native empty RPMSG packet behavior. User accesses occur
outside the device/MM locks, and RX acknowledgements occur outside queue spin
locks. Idle driver unregister runs in a referenced worker because release can
run under the MM write lock. Open admission and idle transitions share the
registration lock; removal cancels the worker and drains any unregister without
holding that lock across MM revocation. Registration failures, interrupted waits,
timeouts, publication failures and minor/device references unwind explicitly.

The GLINK transport signal setter accepts NULL for synchronous detach. The
atomic dispatcher and setter share its existing receive spinlock; callbacks
must remain atomic and must not register/detach themselves recursively.

`verify_glink_pkt.py` compiles 32 actual provider functions under ASan/UBSan.
Its host MM/PTE, transport and workqueue stubs exercise short and multi-page
packets, every byte offset within a page, direct-page identity, allocation/map failures,
foreign ownership, overlaps, stale rollback cookies, partial unmaps, exiting
MMs, reset during read copy, fragmented/nonblocking reads, ioctl faults and
failed/idle opens, callback rejection and zero-length/native writes. A separate threaded test checks actual signal dispatch and
synchronous detach. Byte assertions are counted separately from independent
scenarios. These are host control-flow and memory tests: real DSP mappings,
platform binding, phone boot and playback remain integration/device checks.
