# Native PERF statistics

V2 requests the pinned native vendor algorithm number `5045524653544154`
(decimal) and `PARAM_PROTOCOL_ATTRIBUTE=2`, preserving the exact firmware ABI.
It decodes the native 16-byte address/length response in little endian. Missing
or unsupported firmware metadata remains a real provider error.

The reader preserves the packed `PERF` revision-one table layout and its
word-scaled domain offsets. All header, domain and frequency rows are validated
against the actual mapped length before publication; 16-bit counts are retained.
Domain geometry/frequency caches are complete before debugfs becomes visible.
The `usage` and `time_in_state` text interface is unchanged. Output grows to fit
all rows instead of silently truncating to one page. Sequence retries are bounded
and a continuously busy firmware table returns `-EAGAIN`; changed geometry or
frequency identity returns `-ESTALE` rather than using stale indices.

Each device owns its mapping, cached rows and debugfs tree. Debugfs active readers
are drained before devm memory is freed, and protocol/device references are
balanced after the consumer disappears. No ROM, firmware IDs, authentication or
fuse handling is changed.

`verify_scmi_stats.py` compiles the actual table/map/read functions with MMIO,
allocation and debugfs service stubs. Thirty-six ASan/UBSan cases cover native
64-bit counters and text output, malformed lengths/offsets/counts, changing/busy
sequences, allocation failures, stale tables, and file-reference balancing.
Real firmware address delivery and counter updates require device validation.
