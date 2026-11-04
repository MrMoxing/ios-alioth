/*
 * sleep_test: which wait comes back in this kernel.
 *
 * The reference kernel's Wi-Fi port works until something has to wait: `sleep 30` in a job never
 * returned (wifinotify, 14:41), a dispatch_after block never ran, and a pthread's first sleep came
 * back while its second did not. This asks each primitive in turn, printing a line before and after,
 * so whichever one hangs is the last "before" in the log:
 *
 *     usleep(1000)  usleep(200000)  sleep(1)  sleep(5)  nanosleep(2 s)  select(2 s)  clock_nanosleep
 *
 * Everything is unbuffered and on stdout, which the job's stdout is (/dev/console).
 */
#include <errno.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static mach_timebase_info_data_t timebase;

static uint64_t
now_ms(void)
{
	return mach_absolute_time() * timebase.numer / timebase.denom / 1000000;
}

static void
report(const char *what, uint64_t before)
{
	printf("sleep_test: %s came back after %llu ms\n", what, (unsigned long long)(now_ms() - before));
}

int
main(void)
{
	uint64_t before;
	struct timespec ts;
	struct timeval tv;

	setvbuf(stdout, NULL, _IONBF, 0);
	mach_timebase_info(&timebase);
	printf("sleep_test: begin\n");

	printf("sleep_test: usleep(1000) asked\n");
	before = now_ms();
	usleep(1000);
	report("usleep(1000)", before);

	printf("sleep_test: usleep(200000) asked\n");
	before = now_ms();
	usleep(200000);
	report("usleep(200000)", before);

	printf("sleep_test: sleep(1) asked\n");
	before = now_ms();
	sleep(1);
	report("sleep(1)", before);

	printf("sleep_test: sleep(5) asked\n");
	before = now_ms();
	sleep(5);
	report("sleep(5)", before);

	printf("sleep_test: nanosleep(2 s) asked\n");
	before = now_ms();
	ts.tv_sec = 2;
	ts.tv_nsec = 0;
	if (nanosleep(&ts, NULL) != 0) {
		printf("sleep_test: nanosleep failed: %s\n", strerror(errno));
	}
	report("nanosleep(2 s)", before);

	printf("sleep_test: select(2 s) asked\n");
	before = now_ms();
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	if (select(0, NULL, NULL, NULL, &tv) < 0) {
		printf("sleep_test: select failed: %s\n", strerror(errno));
	}
	report("select(2 s)", before);

	printf("sleep_test: third sleep(1) asked (the second one is where a thread stopped before)\n");
	before = now_ms();
	sleep(1);
	report("sleep(1) again", before);

	printf("sleep_test: end\n");
	return 0;
}
