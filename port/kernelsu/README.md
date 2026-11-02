# KernelSU Next from upstream

Every `build.py WORKSPACE` invocation clones or fetches the **dev** branch from
https://github.com/KernelSU-Next/KernelSU-Next.git, matching the KernelSU Next
source selection in `nethunter-ksunext-upstream:build.sh`. There is no stable tag
or fixed revision selection. Fetch failures stop the build.

`../integrate_kernelsu.py` materializes the fetched kernel source under
`common/drivers/kernelsu`, enables it through upstream Kconfig's `default y`,
and connects `drivers/Makefile` and `drivers/Kconfig`. The build checks the
resulting configuration for built-in `CONFIG_KSU=y`. Manager authentication,
policy profiles and debug defaults retain upstream behavior. SUSFS is not added.

Kleaf runs without source Git metadata or network access. The helper resolves
the version before compilation using upstream's `30000 + git rev-list --count`
algorithm, and resolves the nearest tag for the version label. Only Kbuild's
Git-discovery/version section is replaced with these compiler defines. The shared
top-level `uapi/` headers are materialized at `kernel/include/uapi`, replacing
upstream's relative symlink so Kleaf includes them in its sandbox inputs. A tag
in this label does not mean the source was checked out at that tag.

The workspace caches the checkout in `.kernelsu-next/repo` and records each
selected commit, version, tag and source SHA-256 in `.kernelsu-next/source.json`.
Each successful artifact archives this receipt and its exact source snapshot in
`inputs/kernelsu-next/`; these records document what was built without preventing
the next build from following upstream. Preparation verifies all staged bytes
and rejects unexpected edits before updating. KMI checks remain enabled.

Use a Manager compatible with the fetched upstream revision. Compilation and
offline verification do not establish successful on-device root.
