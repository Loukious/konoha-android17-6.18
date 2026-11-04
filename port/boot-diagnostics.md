# Native boot and suspend diagnostics

`boot_stats.ko` reads the native bootloader IMEM counters and MPM sleep counter,
then releases both mappings. It preserves the native KPI messages and counter
arithmetic. The port rejects disabled/missing nodes, short MMIO resources and a
zero clock frequency before any read. DT/property errors propagate, failed
mapping setup unmaps IMEM, and all node references are released on every path.
No firmware data or timestamps are fabricated.

`msm_show_epoch.ko` preserves both native suspend/resume trace hooks and messages.
Callbacks log their actual arguments without sharing unnecessary global values.
Registration errors propagate; failure of the second hook unregisters and drains
the first. Module removal unregisters and synchronizes both hooks before code
can disappear. Existing GKI hook exports are sufficient; no additional KMI
symbols, ROM policy or device-tree enabling is introduced.

`verify_boot_diagnostics.py` compiles the two complete adapted C sources under
ASan/UBSan. Its 16 cases cover every resource/property/map failure, disabled DT
nodes, successful bounded reads, cleanup, both hook registration failures,
64-bit logging values and unload synchronization. The patch is checked against
both pinned Git blobs with zero-fuzz forward and reverse replay. Shared DDK
compilation and live timestamp behavior remain integration/device checks.
