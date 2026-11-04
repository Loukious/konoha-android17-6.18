# Onyx CPUSS firmware and register access

Read-only partition dumps match the phone SHA256 values in acquisition.json.
Large firmware binaries remain in the local prepared workspace.

HYP configures read access for VMID 3 (HLOS/Android) to the eight CPU, L3 and
APSS 64 KiB windows. Its native 0xc2000501 path forwards the original ID to TZ.
See hyp-cpuss-static-findings.json and the accompanying instruction excerpts.

TZ contains all 18 native config registers in its IO whitelist, flags 0x82820000.
Its IO_READ handler at 0x15326e5c accepts the flagged reads if either of two gates
returns zero, and denies them if both are nonzero. The second gate evaluates
fuse words at 0x221c8780/0x221c8784. The dispatcher converts the handler's negative
result to raw SCM -2, compatible with the observed secure reply. Gate denial is
an explanation supported by static code; neither live gate value was captured,
and the first gate's security-state meaning remains unknown.

At boot f2e3caf9-2423-4d93-801e-1f0927f1b27e, the bounded read-only diagnostic
successfully read all 18 registers through HLOS MMIO, while all SCM reads returned
Linux -22. Eight CPU and L3 count-enable registers read zero. Each CPU residency
config read 0x14 (C3/C4 enabled); cluster residency config read 0x04 (D4 enabled).
No register writes or resets occurred. The diagnostic module was unloaded and
removed from the phone; boot identity was unchanged and SELinux stayed enforcing.

This establishes actual configuration access, not counter advancement or a
complete driver fix. The diagnostic's nofault helper does not guarantee recovery
from every ARM64 external MMIO abort; the read target was selected using HYP
mapping evidence. Subsequent driver and persistence validation must be recorded
separately.
