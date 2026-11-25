/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	kevent() adding a descriptor that cannot be selected (haiku#183, item 2).

	select_fd() notifies the event of a descriptor without select support (a
	directory opened with opendir(), an attribute, an index or a query) at
	once and then fails with B_UNSUPPORTED. The notification
	put the event on the queue's list of pending events, and
	EventQueue::Select() freed it there: the next wait returned an event for
	a registration that had failed, read from freed memory, and closing the
	queue freed it again.

	The test adds a directory many times, alone and while another thread
	waits on the queue, and expects each attempt to fail and to leave
	nothing pending. A pipe added afterwards must work.
*/


#include <syslog.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>


static int sFailures;


static void
check(bool ok, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("%s: ", ok ? "ok" : "FAILED");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	if (!ok)
		sFailures++;
}


struct waiter_data {
	int	queue;
	int	result;
	int	error;
	int	ident;
};


static void*
waiter(void* _data)
{
	waiter_data* data = (waiter_data*)_data;
	struct kevent event;
	memset(&event, 0, sizeof(event));
	timespec timeout = { 0, 300 * 1000 * 1000 };
	data->result = kevent(data->queue, NULL, 0, &event, 1, &timeout);
	data->error = errno;
	data->ident = (int)event.ident;
	return NULL;
}


int
main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	alarm(60);

	int queue = kqueue();
	if (queue < 0) {
		printf("FAILED: kqueue: %s\n", strerror(errno));
		return 1;
	}
	// (the descriptor of an opendir(); a directory opened with open() is a
	// file descriptor to the kernel and has select support)
	DIR* dir = opendir("/boot/system");
	if (dir == NULL) {
		printf("FAILED: opendir: %s\n", strerror(errno));
		return 1;
	}
	int directory = dirfd(dir);

	// alone
	const int kRounds = 200;
	int accepted = 0;
	int pending = 0;
	int lastError = 0;
	for (int i = 0; i < kRounds; i++) {
		struct kevent change;
		EV_SET(&change, directory, EVFILT_READ, EV_ADD, 0, 0, NULL);
		if (kevent(queue, &change, 1, NULL, 0, NULL) == 0)
			accepted++;
		else
			lastError = errno;

		struct kevent event;
		timespec noWait = { 0, 0 };
		if (kevent(queue, NULL, 0, &event, 1, &noWait) != 0)
			pending++;
	}
	if (accepted == kRounds) {
		printf("skipped: kevent() accepts an opendir() descriptor here, "
			"there is no failing registration to test\n");
		return 0;
	}
	check(accepted == 0, "%d registrations of an opendir() descriptor, %d "
		"accepted (the others: %s)", kRounds, accepted, strerror(lastError));
	check(pending == 0, "%d waits after a failed registration returned an "
		"event", pending);

	// with another thread waiting on the queue
	int woken = 0;
	for (int i = 0; i < 20; i++) {
		waiter_data data = { queue, 0, 0, -1 };
		pthread_t thread;
		pthread_create(&thread, NULL, waiter, &data);
		usleep(20000);
		for (int k = 0; k < 10; k++) {
			struct kevent change;
			EV_SET(&change, directory, EVFILT_READ, EV_ADD, 0, 0, NULL);
			kevent(queue, &change, 1, NULL, 0, NULL);
		}
		pthread_join(thread, NULL);
		if (data.result != 0)
			woken++;
	}
	check(woken == 0, "%d of 20 waits of another thread ended with an event "
		"or an error during failed registrations", woken);

	// the queue still works
	int fds[2];
	if (pipe(fds) != 0) {
		printf("FAILED: pipe: %s\n", strerror(errno));
		return 1;
	}
	struct kevent change;
	EV_SET(&change, fds[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
	int result = kevent(queue, &change, 1, NULL, 0, NULL);
	check(result == 0, "a pipe is added: %s",
		result == 0 ? "no error" : strerror(errno));

	struct kevent event;
	memset(&event, 0, sizeof(event));
	timespec shortWait = { 0, 50 * 1000 * 1000 };
	result = kevent(queue, NULL, 0, &event, 1, &shortWait);
	check(result == 0, "nothing is pending for the empty pipe (%d)", result);

	write(fds[1], "x", 1);
	timespec wait = { 2, 0 };
	result = kevent(queue, NULL, 0, &event, 1, &wait);
	check(result == 1 && (int)event.ident == fds[0],
		"the written pipe is reported (%d event, descriptor %d, expected "
		"%d)", result, (int)event.ident, fds[0]);

	close(queue);
	close(fds[0]);
	close(fds[1]);
	closedir(dir);

	// closing a queue freed the event a second time: give the kernel's
	// allocator something to do with that memory
	for (int i = 0; i < 50; i++) {
		int other = kqueue();
		if (other < 0)
			break;
		int otherFDs[2];
		if (pipe(otherFDs) == 0) {
			EV_SET(&change, otherFDs[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
			kevent(other, &change, 1, NULL, 0, NULL);
			close(otherFDs[0]);
			close(otherFDs[1]);
		}
		close(other);
	}
	check(true, "50 more queues were created and closed");

	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	syslog(LOG_INFO, "kqueue_unsupported_fd_test: return sFailures == 0 ? 0 : 1;");
	return sFailures == 0 ? 0 : 1;
}
