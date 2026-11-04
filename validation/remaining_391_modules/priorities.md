# Remaining Onyx module queue

The accepted checkpoint contains **391 platform / 471 packaged modules** on the
single development Onyx / SM8735 phone. No missing fundamental driver was found
for demonstrated ordinary operation. These 79 retained names are an applicability
queue; their absence does not prove a missing feature. Broader feature, power
and endurance validation remains.

Normal reboot kernel/pmsg archival passed; automatic block2mtd attachment waits
for ROM policy patch 0018. `xhci_sideband` is already built into GKI. Guest/HVC
provisioning and guest-dependent `dmesg_dumper` remain deferred. China-only
notification LED work is outside active scope.

See the [current runtime summary](../onyx-runtime-summary-20261104.md) and
[sanitized source inventory](priorities.json). Baseline source paths are relative
to the build workspace and pinned by [the source manifest](../../port/sources.json).
Both Tuna device-tree variants pass offline checks; other phone/board support is
unestablished. Raw device and machine-specific receipts remain local.

| Candidate | Priority |
| --- | --- |
| `mhi_dev_satellite` | P4 — hold: hardware or feature use unconfirmed |
| `bootmonitor` | P3 — low: logging, diagnostics, tracing and dependencies |
| `chipreg` | P3 — low: logging, diagnostics, tracing and dependencies |
| `coresight_stm` | P3 — low: logging, diagnostics, tracing and dependencies |
| `coresight_tmc_sec` | P3 — low: logging, diagnostics, tracing and dependencies |
| `coresight_uetm` | P3 — low: logging, diagnostics, tracing and dependencies |
| `diag` | P3 — low: logging, diagnostics, tracing and dependencies |
| `dmesg_dumper` | P3 — low: logging, diagnostics, tracing and dependencies |
| `glink_probe` | P4 — hold: hardware or feature use unconfirmed |
| `gpu_dump_skip_cdev` | P4 — hold: hardware or feature use unconfirmed |
| `ipclite_test` | P3 — low: logging, diagnostics, tracing and dependencies |
| `mtd_blkdevs` | P3 — low: logging, diagnostics, tracing and dependencies |
| `mtdblock` | P3 — low: logging, diagnostics, tracing and dependencies |
| `ofpart` | P3 — low: logging, diagnostics, tracing and dependencies |
| `qcom_dynamic_ramoops` | P4 — hold: hardware or feature use unconfirmed |
| `rdbg` | P3 — low: logging, diagnostics, tracing and dependencies |
| `stm_console` | P3 — low: logging, diagnostics, tracing and dependencies |
| `stm_core` | P3 — low: logging, diagnostics, tracing and dependencies |
| `stm_ftrace` | P3 — low: logging, diagnostics, tracing and dependencies |
| `stm_heartbeat` | P3 — low: logging, diagnostics, tracing and dependencies |
| `stm_p_ost` | P3 — low: logging, diagnostics, tracing and dependencies |
| `thermal_config` | P3 — low: logging, diagnostics, tracing and dependencies |
| `tz_log_dlkm` | P3 — low: logging, diagnostics, tracing and dependencies |
| `clk_scmi` | P4 — hold: hardware or feature use unconfirmed |
| `cqhci` | P4 — hold: hardware or feature use unconfirmed |
| `ddr_cdev` | P4 — hold: hardware or feature use unconfirmed |
| `i3c_master_msm_geni` | P4 — hold: hardware or feature use unconfirmed |
| `leds_awrgb21024` | P4 — hold: hardware or feature use unconfirmed |
| `leds_qcom_flash` | P4 — hold: hardware or feature use unconfirmed |
| `lt9611uxc` | P4 — hold: hardware or feature use unconfirmed |
| `max31760_fan` | P4 — hold: hardware or feature use unconfirmed |
| `mem_offline` | P4 — hold: hardware or feature use unconfirmed |
| `mhi_dev_uci` | P4 — hold: hardware or feature use unconfirmed |
| `nb7vpq904m` | P4 — hold: hardware or feature use unconfirmed |
| `phy_msm_m31_eusb2` | P4 — hold: hardware or feature use unconfirmed |
| `phy_qcom_emu` | P4 — hold: hardware or feature use unconfirmed |
| `phy_qcom_ufs_qrbtc_sdm845` | P4 — hold: hardware or feature use unconfirmed |
| `pinctrl_spmi_mpp` | P4 — hold: hardware or feature use unconfirmed |
| `ptp_kvm` | P4 — hold: hardware or feature use unconfirmed |
| `q2spi_geni` | P4 — hold: hardware or feature use unconfirmed |
| `qcom_glink_spss` | P4 — hold: hardware or feature use unconfirmed |
| `qcom_hv_haptics` | P4 — hold: hardware or feature use unconfirmed |
| `qcom_spss` | P4 — hold: hardware or feature use unconfirmed |
| `qmp_dlkm` | P4 — hold: hardware or feature use unconfirmed |
| `qsee_ipc_irq_bridge` | P4 — hold: hardware or feature use unconfirmed |
| `qti_dmof_scmi` | P4 — hold: hardware or feature use unconfirmed |
| `qti_ocp_notifier` | P4 — hold: hardware or feature use unconfirmed |
| `sc7601` | P4 — hold: hardware or feature use unconfirmed |
| `sc8541` | P4 — hold: hardware or feature use unconfirmed |
| `snd_soc_hdmi_codec` | P4 — hold: hardware or feature use unconfirmed |
| `spcom` | P4 — hold: hardware or feature use unconfirmed |
| `spss_utils` | P4 — hold: hardware or feature use unconfirmed |
| `xhci_pci_renesas` | P4 — hold: hardware or feature use unconfirmed |
| `mca_basic_wireless` | Skip — wireless charging absent |
| `mca_quick_wireless` | Skip — wireless charging absent |
| `mca_wireless_revchg` | Skip — wireless charging absent |
| `nuvolta_1652` | Skip — wireless charging absent |
| `sc96281_comp` | Skip — wireless charging absent |
| `qcom_amoled_regulator` | Skip — disabled Onyx DT hardware |
| `qti_amoled_ecm` | Skip — disabled Onyx DT hardware |
| `radio_i2c_rtc6226_qca` | Skip — disabled Onyx DT hardware |
| `sdhci_msm` | Skip — disabled Onyx DT hardware |
| `cambistmclkcc_sun` | Skip — alternate Sun/Kera SoCs |
| `camcc_kera` | Skip — alternate Sun/Kera SoCs |
| `camcc_sun` | Skip — alternate Sun/Kera SoCs |
| `debugcc_kera` | Skip — alternate Sun/Kera SoCs |
| `debugcc_sun` | Skip — alternate Sun/Kera SoCs |
| `dispcc_sun` | Skip — alternate Sun/Kera SoCs |
| `evacc_sun` | Skip — alternate Sun/Kera SoCs |
| `gcc_kera` | Skip — alternate Sun/Kera SoCs |
| `gcc_sun` | Skip — alternate Sun/Kera SoCs |
| `gpucc_kera` | Skip — alternate Sun/Kera SoCs |
| `gpucc_sun` | Skip — alternate Sun/Kera SoCs |
| `pinctrl_kera` | Skip — alternate Sun/Kera SoCs |
| `pinctrl_sun` | Skip — alternate Sun/Kera SoCs |
| `qnoc_kera` | Skip — alternate Sun/Kera SoCs |
| `qnoc_sun` | Skip — alternate Sun/Kera SoCs |
| `tcsrcc_sun` | Skip — alternate Sun/Kera SoCs |
| `videocc_sun` | Skip — alternate Sun/Kera SoCs |
