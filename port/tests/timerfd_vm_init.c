/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/io.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void result(const char *name, int ok)
{
	printf("TIMERFD_API %s %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok)
		failures++;
}

static void expiry_test(clockid_t clock)
{
	struct itimerspec spec = { .it_value.tv_nsec = 50000000 };
	struct pollfd pfd = { .events = POLLIN };
	uint64_t ticks = 0;
	int fd = timerfd_create(clock, TFD_NONBLOCK | TFD_CLOEXEC);
	char name[80];

	snprintf(name, sizeof(name), "clock-%d-expiry", clock);
	if (fd < 0) {
		result(name, 0);
		return;
	}
	pfd.fd = fd;
	result(name, timerfd_settime(fd, 0, &spec, NULL) == 0 &&
		poll(&pfd, 1, 2000) == 1 &&
		read(fd, &ticks, sizeof(ticks)) == sizeof(ticks) && ticks >= 1);
	close(fd);
}

static void cancel_test(void)
{
	struct itimerspec disarmed = {0};
	struct epoll_event event = { .events = EPOLLIN | EPOLLWAKEUP };
	struct timespec original, changed;
	uint64_t ticks;
	int fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
	int ep = epoll_create1(EPOLL_CLOEXEC);
	int setup = fd >= 0 && ep >= 0;

	if (setup)
		setup = timerfd_settime(fd, TFD_TIMER_ABSTIME |
			TFD_TIMER_CANCEL_ON_SET, &disarmed, NULL) == 0 &&
			epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) == 0 &&
			clock_gettime(CLOCK_REALTIME, &original) == 0;
	result("disarmed-cancel-on-set-setup", setup);
	if (setup) {
		changed = original;
		changed.tv_sec += 2;
		int changed_ok = clock_settime(CLOCK_REALTIME, &changed) == 0;
		int delivered = epoll_wait(ep, &event, 1, 2000) == 1;
		ssize_t n = read(fd, &ticks, sizeof(ticks));
		result("disarmed-clock-change-cancels", changed_ok && delivered &&
			n == -1 && errno == ECANCELED);
		/* Re-arm after consumption and verify a second change is delivered. */
		int rearmed = timerfd_settime(fd, TFD_TIMER_ABSTIME |
			TFD_TIMER_CANCEL_ON_SET, &disarmed, NULL) == 0;
		changed_ok = clock_settime(CLOCK_REALTIME, &original) == 0;
		delivered = epoll_wait(ep, &event, 1, 2000) == 1;
		n = read(fd, &ticks, sizeof(ticks));
		result("second-clock-change-cancels", rearmed && changed_ok &&
			delivered && n == -1 && errno == ECANCELED);
	}
	if (fd >= 0)
		close(fd);
	if (ep >= 0)
		close(ep);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	printf("TIMERFD_API_BEGIN\n");
	expiry_test(CLOCK_MONOTONIC);
	expiry_test(CLOCK_BOOTTIME);
	expiry_test(CLOCK_REALTIME);
	expiry_test(CLOCK_BOOTTIME_ALARM);
	expiry_test(CLOCK_REALTIME_ALARM);
	cancel_test();
	printf("TIMERFD_API_END failures=%d\n", failures);
	sync();
	if (ioperm(0xf4, 4, 1) == 0)
		outb(failures ? 0x11 : 0x10, 0xf4);
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
