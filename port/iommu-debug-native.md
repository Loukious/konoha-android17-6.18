# Native IOMMU diagnostics

`qcom_iommu_debug.ko` is integrated as `//onyx:qcom_iommu_debug` and loads through
the existing vendor module loader. The complete pinned Xiaomi driver retains all
eleven diagnostic interfaces and seven Onyx use cases. Default probe publishes
metadata without selecting a use case or initiating DMA.

Native DMA API mappings, coherent CPU access and SMMU ATOS translations passed on
the exclusive apps test stream SID 0x400, including 4 KiB, 64 KiB and 1 MiB
map/unmap cleanup. Peripheral DMA payload transfer, secure/GPU use cases,
full-VA stress and fault injection were not tested. These checks establish the
bounded mapping/translation path, not every peripheral's DMA operation.

The port fixes debugfs ownership, partial-probe rollback, child references,
input/address bounds, page-by-page translation checks and allocation cleanup.
Mappings retain backing ownership until their original tuple is retired. Failed
unmap blocks use-case reset; removal retains uncertain backing pages until reboot.
Local actual-C ASan/UBSan validation covered 204,148 cases and rejected seven
mutations; 87 production import checks and pinned source replay passed. That local
harness is not claimed as a portable CI input.

Two temporary and two installed metadata/control/lifecycle cycles covered
unselected use cases, iteration bounds, invalid selectors, open-file unload
protection, stale reads, unbind/rebind and normal unload. Those earlier cycles
performed no mapping; the dedicated test-stream mapping checks followed later.
Native reboot loading, Android/audio, LPM defaults and partition identities passed.
See the [current runtime summary](../validation/onyx-runtime-summary-20261104.md).
Raw traces, mapping receipts and machine-specific installation evidence remain local.
