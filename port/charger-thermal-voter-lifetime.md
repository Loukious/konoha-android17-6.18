# Charger thermal voter lifetime

The charger thermal consumer now keeps supplier names and uses the existing
`mca_vote_named()` API. That API holds the voter registry's SRCU read lock
through lookup and use. The former cached `mca_votable *` pointers could outlive
their suppliers when shutdown removed a charger strategy first.

Disabling a vote after its supplier has already been removed succeeds because
that supplier has discarded its votes. Missing suppliers still return errors
when enabling a vote, and other I/O errors remain visible. The change is
replayable as [patch 0244](patches/0244-fix-thermal-voter-lifetime-during-shutdown.patch)
and is included in the source ledger and build verification.

[Host validation](charger-thermal-voter-host-validation.json) covers four
supplier-before-consumer shutdown cases, ordinary teardown, active setter
removal, deferred voter discovery, registration failures and level bounds under
ASan/UBSan. The same regression
[rejects the original driver](charger-thermal-voter-regression-validation.json)
for using a destroyed supplier. All 53 imported symbols match the installed
kernel/providers.

The retained module SHA256 is
`f3e9c8bffa71aef8de2ff3b72a4b7c6bebe76991af7b754434fa13fd92fbd3c2`,
build ID `254b7b72efd10b09009792eb5f8a93fce4352b42`.
The final [parallel-loading device validation](../validation/parallel-module-loading-20261105.json)
checks that identity in Android and recovery, and verifies the complete images
after returning to Android. The thermal policy and audio modules are retained
through the separate WALT readiness repair. Host mocks exercise lifetime and
shutdown ordering; device checks establish the installed binary and successful
reboot paths.
