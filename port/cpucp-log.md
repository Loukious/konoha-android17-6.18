# Native CPUCP/PDP firmware logging

`cpucp_log.ko` keeps the native CPUCP marker-based two-buffer layout and PDP
upper-word address/capacity fields. Payloads are validated before taking a pool
buffer; malformed markers cannot underflow or drain the free list. Incoming PDP
messages are not modified. Log chunks are copied into bounded, terminated trace
strings; residual fragments remain bounded to the native 1024-byte prefix space.
Tracepoints retain their original CPUCP/PDP names, with the 6.18 `__assign_str`
syntax.

Firmware controls retain `CPUCPCTL=0x435055435043544c`, parameters 1/3/5 and exact
native u64 or packed module-ID/log-level responses. Module enumeration is bounded
to 128 responses and names to 32 bytes. Provider errors are returned without
partial success. Writes require two valid u32 fields and a terminated copy of
the user input; rejected mailbox enables leave local state unchanged.

Each device owns its workqueue, buffers, controls, debugfs child and acquired SCMI
protocol reference. Initialization completes before mailbox RX is requested.
Removal drains debugfs, serializes control stop, releases channels (the existing
qcom_cpucp provider locks callbacks against channel shutdown), cancels work, then
releases memory and protocol references. PDP debugfs parent ownership is counted,
so removing one endpoint keeps the other endpoint's child alive.

Pinned `sm8735-devicetrees/qcom/sun.dtsi` supplies `qcom,log-type=<0>` for
`qcom,cpucp-log`; pinned `tuna.dtsi` uses that same compatible without the property.
An absent property defaults to the same native CPUCP enum/type zero only for that
compatible. Explicit malformed properties fail. PDP endpoints keep explicit
native type-one/type-two properties.

`verify_cpucp_log.py` runs 41 actual-C ASan/UBSan checks for marker bounds, retained
pool buffers, native PDP decoding, residual/unterminated trace chunks, firmware
errors and bounded enumeration, input parsing, mailbox transaction failures,
resource geometry and idempotent teardown. Physical firmware delivery remains a
separate device check. No ROM or firmware/authentication/fuse behavior is changed.
