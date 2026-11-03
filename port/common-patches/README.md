# Required common-kernel fixes

`build.py` applies these fixes on every build after KernelSU integration. The
patch and manifest hashes must match the reviewed input and output sources.
The build archives the patched files in `common-fixes/source/` and restores
the pinned checkout afterward, preserving the KernelSU verification contract.
This is the persistent production fix path; `--qdss-diagnostic` independently
controls the temporary AMBA/clock logging.

`0001-required-common-fixes.patch` combines the nine-driver removal fix from
[Jie Gan's July v3 series](https://lists.openwall.net/linux-kernel/2026/07/10/60).
The CoreSight portion matches
[patch 1/2](https://lists.openwall.net/linux-kernel/2026/07/10/62) exactly.
Imported Onyx funnel code receives the same callback change through patch
`0175` in the normal source staging pipeline.

[Patch 2/2](https://lists.openwall.net/linux-kernel/2026/07/10/61) changes
`itnoc_remove()`, which does not exist in the pinned 6.18 tree. Its TNOC driver
uses AMBA clock ownership and remains unchanged. Both original messages are
preserved under `validation/clock_suspend_resume/`.

The same patch adds `thermal_device_register()` and its GPL export for the
Xiaomi thermal policy module. It creates devices through the thermal class
without exposing private class internals. `required-common-fixes.patch` keeps
the complete replay input so CI can preserve local thermal changes as LTS
integrates the CoreSight fixes. See `../mi-thermal-interface.md` for the ABI and
validation details.

The removal correction resumes the platform device before teardown, disables
runtime PM, marks it suspended, and drops the usage vote without suspending
again. Devres then releases the enabled clock vote once. The submitted patch
ignores the result of `pm_runtime_get_sync()`; clock balance therefore assumes
successful runtime resume. Lifecycle tests preserve and document that limit.

This series does not include the separately discussed September platform
probe-failure fix. Neither removal tests nor a clean startup establish the
cause of the historical warnings from `amba_pm_runtime_suspend()`.

The native CTI port also protects the shared CTI association callbacks with
SRCU. Register/unregister callbacks can sleep while changing the graph;
`coresight_remove_cti_ops()` publishes NULL and drains existing callbacks before
the CTI module can unload. It holds no graph mutex while waiting. The actual
wrappers have concurrent add/remove-versus-unload host tests in
`verify_cti_assoc.py`, and the normal GKI build checks the real SRCU API.
