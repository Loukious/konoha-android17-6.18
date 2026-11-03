# Native MCA misc business provider

The exact pinned `mca_business_misc_comp` sources are available as a DDK module
under the existing `CONFIG_XM_POWER_SUPPLY=m`. Both checked Onyx trees keep its
node disabled; packaging the module does not enable that node or add hardware.

The native `AWAKE` OR election keeps its real `pm_stay_awake`/`pm_relax` callback.
Probe creates the device wakeup source before publishing voters. The native
`SMART_BATT` MIN election keeps its name, but its callback returns `EOPNOTSUPP`:
the shipped callback has no smart-battery provider operation. It cannot claim
that a requested battery operation succeeded. Authentication and fuse controls
are unaffected.

Allocation, wakeup, action-registration and either voter-publication failure
reach the caller with the original error. A failure creating the second voter
destroys the first. Remove and shutdown release the managed cleanup action once,
unpublish both named elections, drain named-vote SRCU callbacks through the
existing voter provider, relax wakeup and destroy its source before devm data.
This driver does not retain external raw voter pointers or own periodic work.

`verify_business_misc.py` compiles both complete adapted translation units and
the actual header. Its 119 ASan/UBSan cases cover all initialization failures,
partial voter rollback, real awake/relax state, unsupported smart-battery votes,
shutdown followed by removal, and managed allocation release. Voter/wakeup
stubs establish control flow; they do not operate hardware or replace the
existing kernel voter concurrency validation.
