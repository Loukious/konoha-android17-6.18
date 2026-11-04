# Native battery diagnostics

The installed native driver returns 2,448-byte QBG replies. Aggregation uses the
stock-compatible 16-byte request, including its final zero word, but still times
out with votables_status unavailable (-110). The cause remains unresolved in the
tested kernel/ROM/firmware combination. No charging override or guard reset was
performed; useful aggregation/voting behavior is not established.

Patch 0231 fixes keyed reply ownership and teardown. Patch 0234 preserves the
full native QBG interface when aggregation fails and rejects malformed matching
replies without publishing invalid data. Patch 0236 corrects the stock request
layout. The portable actual-C driver/transport harness passes 10,575 ASan/UBSan
cases and rejects 19 mutations; run `python3 port/verify_battery_debug.py`.
Kernel frameworks and firmware are mocked in those tests.

Native autoload, binding, fresh QBG requests, normal load/unload and stale-file
behavior have bounded live coverage. The public [runtime summary](../validation/onyx-runtime-summary-20261104.md) records current status and limits.
Only payload lengths and hashes are retained in publication notes. Static
firmware readiness analysis does not measure the live gate or establish a
firmware-only cause; raw firmware and device receipts remain local.
