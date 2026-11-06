/* SPDX-License-Identifier: GPL-2.0 */
/* Test-only include for fs/timerfd.c in the disposable VM kernel. */
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <trace/events/workqueue.h>

static struct work_struct timerfd_work;
static void timerfd_resume_work(struct work_struct *work);
void timerfd_resume(void);

static atomic_t resume_test_callbacks = ATOMIC_INIT(0);
static DECLARE_COMPLETION(resume_test_ran);

static void resume_test_observe(void *unused, struct work_struct *work)
{
	if (work->func != timerfd_resume_work)
		return;
	atomic_inc(&resume_test_callbacks);
	complete(&resume_test_ran);
}

static int resume_test_init(struct kunit *test)
{
	flush_work(&timerfd_work);
	atomic_set(&resume_test_callbacks, 0);
	reinit_completion(&resume_test_ran);
	return register_trace_workqueue_execute_start(resume_test_observe, NULL);
}

static void resume_test_exit(struct kunit *test)
{
	flush_work(&timerfd_work);
	unregister_trace_workqueue_execute_start(resume_test_observe, NULL);
	tracepoint_synchronize_unregister();
}

static void resume_notification_while_awake(struct kunit *test)
{
	timerfd_resume();
	flush_work(&timerfd_work);
	KUNIT_EXPECT_EQ(test, atomic_read(&resume_test_callbacks), 1);
}

static void resume_notification_waits_for_thaw(struct kunit *test)
{
	unsigned int i;
	unsigned long premature;
	int callbacks_while_frozen;
	bool busy;

	/* Exercise the real workqueue freezer, not a replacement queue model. */
	freeze_workqueues_begin();
	for (i = 0; i < 100 && freeze_workqueues_busy(); i++)
		msleep(10);
	busy = freeze_workqueues_busy();
	if (busy) {
		thaw_workqueues();
		KUNIT_FAIL(test, "Could not freeze workqueues in the test VM");
		return;
	}

	/* Repeated CPU idle exits must coalesce until actual system resume. */
	for (i = 0; i < 100; i++)
		timerfd_resume();
	premature = wait_for_completion_timeout(&resume_test_ran,
						msecs_to_jiffies(100));
	callbacks_while_frozen = atomic_read(&resume_test_callbacks);

	/* Always thaw before reporting failures, including on the old kernel. */
	thaw_workqueues();
	flush_work(&timerfd_work);
	KUNIT_EXPECT_EQ(test, premature, 0UL);
	KUNIT_EXPECT_EQ(test, callbacks_while_frozen, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&resume_test_callbacks), 1);
}

static void resume_notification_second_freeze(struct kunit *test)
{
	resume_notification_waits_for_thaw(test);
	atomic_set(&resume_test_callbacks, 0);
	reinit_completion(&resume_test_ran);
	resume_notification_waits_for_thaw(test);
}

static struct kunit_case resume_notification_cases[] = {
	KUNIT_CASE(resume_notification_while_awake),
	KUNIT_CASE(resume_notification_waits_for_thaw),
	KUNIT_CASE(resume_notification_second_freeze),
	{}
};

static struct kunit_suite resume_notification_suite = {
	.name = "timerfd-resume-freezer",
	.init = resume_test_init,
	.exit = resume_test_exit,
	.test_cases = resume_notification_cases,
};

kunit_test_suite(resume_notification_suite);
