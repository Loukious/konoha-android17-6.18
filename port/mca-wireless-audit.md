# Remaining native MCA wireless strategies

This source/dependency audit covers the pinned native revision
`10570812a4878b644e31b47c2f0bd052664cf86d`, checked against staged provider
exports and both previously checked `tuna`/`tuna7` DTs. It does not port the
wireless strategies or establish binary ABI compatibility.

All three strategy nodes and `/soc/mca_platform_wireless` are disabled in both
trees. The active business charger has `support-wls=0`. Future build availability
must preserve those settings and cannot substitute a fabricated wireless IC,
authentication result, firmware success or measurement.

The direct source-level ordering is reverse charging, basic wireless, then quick
wireless. The reverse-charging strategy's referenced MCA/provider exports already
exist in the staged tree. Basic wireless adds one missing reverse-charging
export: `mca_wireless_rev_set_firmware_state`. Quick wireless requires five basic
exports: `strategy_class_wireless_op_get_rx_iout_limit`,
`strategy_class_wireless_ops_get_adapter_charger_mode`,
`strategy_class_wireless_ops_get_adapter_power`,
`strategy_class_wireless_ops_set_parallel_charge`, and
`strategy_wireless_enable_cp_error_irq`. Existing FG, buck, CP, load-switch,
wireless class, protocol, strategy, smartcharge, charge-interface, event, voter,
sysfs, workqueue and ADSP-glink providers supply the other referenced MCA exports.
Header/DDK dependencies and kernel symbol CRCs still require build validation.

The strategies need substantive lifecycle work before importing them:

| Native module | Source lines | Work items | Created voters | Concrete teardown gaps |
| --- | ---: | ---: | ---: | --- |
| `mca_wireless_revchg` | 2467 | 16 | 1 | Remove only closes sysfs; reboot notifier, strategy callbacks, global pointer, voters, work and wakeup remain. |
| `mca_basic_wireless` | 4970 | 20 | 15 | Remove stops only the IRQ work and sysfs; other work, strategy/charge/smartcharge callbacks, voters, global pointer, queue and battery power-supply reference remain. |
| `mca_quick_wireless` | 3111 | 1 | 4 | Remove/shutdown are empty; strategy/charge/smartcharge callbacks, monitor work and voters remain. |

The existing providers expose synchronous unregister APIs for strategy,
charge-interface and smartcharge callbacks. Ports must handle registration errors,
unpublish exported global helper pointers with a sleeping-reader lifetime scheme,
drain every callback/work item before freeing device data, and unwind every
partial publication. The strategies cache external raw voter pointers; those
references need conversion to existing named-vote APIs to survive owner removal.
Owned voters must be destroyed only after dependent work/callbacks stop.

Native probe paths ignore several callback-registration, sysfs, wakeup,
power-supply and provider-read errors. Reverse charging also continues publishing
after DT parsing fails. Native firmware update, authentication, boost selection,
current limits, timeouts and failure paths must retain real provider errors and
hardware checks. No wireless strategy has been built or tested by this audit.
