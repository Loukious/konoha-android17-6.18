# Required common-kernel fixes

`build.py` applies these fixes on every build after KernelSU integration. The
patch and manifest hashes must match the reviewed input and output sources.
The build archives the patched files in `common-fixes/source/` and restores
the pinned checkout afterward, preserving the KernelSU verification contract.
This is the persistent production fix path; `--qdss-diagnostic` independently
controls the temporary AMBA/clock logging.

The first patch backports the nine-driver removal fix from
[Jie Gan's July v3 series](https://lists.openwall.net/linux-kernel/2026/07/10/60).
The common-driver diff matches
[patch 1/2](https://lists.openwall.net/linux-kernel/2026/07/10/62) exactly.
Imported Onyx funnel code receives the same callback change through patch
`0175` in the normal source staging pipeline.

[Patch 2/2](https://lists.openwall.net/linux-kernel/2026/07/10/61) changes
`itnoc_remove()`, which does not exist in the pinned 6.18 tree. Its TNOC driver
uses AMBA clock ownership and remains unchanged. Both original messages are
preserved under `validation/clock_suspend_resume/`.

The removal correction resumes the platform device before teardown, disables
runtime PM, marks it suspended, and drops the usage vote without suspending
again. Devres then releases the enabled clock vote once. The submitted patch
ignores the result of `pm_runtime_get_sync()`; clock balance therefore assumes
successful runtime resume. Lifecycle tests preserve and document that limit.

This series does not include the separately discussed September platform
probe-failure fix. Neither removal tests nor a clean startup establish the
cause of the historical warnings from `amba_pm_runtime_suspend()`.
