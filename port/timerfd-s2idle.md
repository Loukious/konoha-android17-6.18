# timerfd resume notification during s2idle

An Onyx trace captured a kernel-generated timerfd cancellation notification
ending s2idle while userspace was still frozen. The notification ran on an
ordinary workqueue after a CPU briefly left idle. Its epoll watcher used
`EPOLLWAKEUP`, so notifying the watcher activated a wake source and ended the
system sleep attempt.

The phone was running Linux `6.18.38-android17-5-maybe-dirty-4k`. Caffeine was
off, the screen was locked, and USB was disconnected for the capture. USB and
charging wake locks had prevented system suspend in an earlier connected test.

Of 134 complete `machine_suspend` spans retained in the unplugged trace, 118
had `timerfd_resume_work` as the first recorded wake-source activation, three
had `qrtr_ws`, and 13 had no retained activation. Some per-CPU buffers wrapped;
these counts describe retained events, not an exhaustive wake history.

One complete example, with times from the same trace:

| Timestamp | Event |
| --- | --- |
| 43614.030634 | `machine_suspend[1] begin` |
| 43614.030670 | `timekeeping_freeze[1] begin` |
| 43614.341538 | `timekeeping_freeze[1] end` |
| 43614.341743 | `[timerfd]` wake-source activation |
| 43614.341989 | `machine_suspend[1] end` |

The wake-source stack included `timerfd_resume_work`, `timerfd_clock_was_set`,
`ep_poll_callback` and `__pm_stay_awake`. Device resume and userspace thaw
followed. AlarmManager's disarmed `CLOCK_REALTIME` timerfd had
`TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET`; its epoll registration included
`EPOLLWAKEUP`. This is Android's clock-change watcher, not an expiring alarm.

[tick_unfreeze()](../common/kernel/time/tick-common.c) resumes timekeeping on
the first CPU to leave s2idle. [timekeeping_resume()](../common/kernel/time/timekeeping.c)
then calls [timerfd_resume()](../common/fs/timerfd.c). A CPU idle exit does not
by itself mean that the system should finish suspending. The old
`schedule_work()` call nevertheless let this userspace notification execute
while the suspend freezer was active.

The fix queues this one work item on `system_freezable_wq`. Workqueue freezing
holds it pending until thaw, and repeated resume requests coalesce. It still
executes normally when workqueues are awake. The work item is static and holds
no saved timerfd context pointers; the existing RCU walk and per-context locks
protect timerfd lifetime when the notification is eventually delivered.
The suspend path thaws workqueues during actual system resume or suspend
unwind, so no new explicit flush or userspace dependency is introduced.

Timer expiration and explicit clock-setting calls retain their existing paths.
This correction does not change the ROM, AlarmManager, parallel module
loading, WLAN roaming policy, firmware replies, PM timeout values or error
propagation.

The WLAN messages have a separate, precise meaning:

| Value | Meaning |
| --- | --- |
| WoW NACK reason 7 | `WMI_WOW_NON_ACK_REASON_ROAM`: firmware roaming holds a lock. |
| PMO status 3 | `QDF_STATUS_E_AGAIN`. |
| Linux status -11 | `-EAGAIN`, abort this suspend attempt and retry. |

The original retained log contains a reason-7 refusal. The later unplugged
trace did not reproduce reason 7: it retained one reason-0 NACK, followed by
a successful firmware ACK 1.147344 seconds later. The inspected WLAN
suspend/ACK code matches the pinned vendor baseline; the newer Xiaomi source
checked for this investigation has the same PMO suspend implementation.
There is no evidence here of a stuck host PM reference or a Link to PC change
to WLAN suspend handling. Indirect effects of application network activity
have not been excluded.

This patch addresses the demonstrated kernel wakeup path and the resulting
extra suspend/resume cycles. It does not establish that all firmware roaming
refusals will disappear. Suppressing or ignoring a firmware NACK would allow
the host to suspend before firmware is ready and is not a valid fix.

The disposable QEMU comparison exercises the actual kernel workqueue freezer
and actual timerfd code, with a test-only include in `fs/timerfd.c`:

| Check | Baseline | Candidate |
| --- | --- | --- |
| Awake resume callback executes | Pass | Pass |
| 100 resume requests stay pending until thaw, then execute once | Fails: callback runs while frozen | Pass |
| Repeated freeze/thaw cycles | Fails: callback runs while frozen | Pass |
| Expiry on MONOTONIC, BOOTTIME, REALTIME and both ALARM clocks | 5 pass | 5 pass |
| Disarmed CANCEL_ON_SET setup and two explicit clock changes | 3 pass | 3 pass |

The tests freeze only workqueues inside a disposable VM. They do not freeze
the host or change its clock. Reproduce with a static C runtime, the normal
kernel build dependencies, and QEMU installed:

```sh
python3 port/tests/run_timerfd_vm.py \
  /path/to/unmodified/common /path/to/patched/common /path/to/new-vm-output
```

The runner requires the original kernel to fail the two freezer checks and
the candidate to pass all three. It also requires all eight timerfd API checks
to pass on both kernels, and saves logs, source/configuration/image hashes and
a report. The VM tests do not establish phone sleep residency or WLAN firmware
behavior after the change. The test include is never used by the production
ARM64 build. Installation and another unplugged phone capture are still
needed for device validation.

The replay component is
[timerfd-resume-freezer.patch](common-patches/timerfd-resume-freezer.patch).
The aggregate and common-source manifest include its exact input/output hashes.
Replay verification checked all 21 affected files and their exact restoration
after the build context exited.

The [validation receipt](validation/timerfd-resume-20261106.json) records the
reproducible VM results, complete 91,829-file source replay and a successful
production ARM64 Kleaf build with strict KMI and symbol-list checks. That build
uses the current recipe's KernelSU revision `b2c1ad00b755`.
The running phone's saved Image hash instead matches the earlier accepted
baseline with KernelSU `a6c81fa4a8b7`. A device comparison must use that saved
baseline's configuration and source inputs to isolate the timerfd change.
