/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	syslog() with a message longer than its buffer (haiku#160).

	send_syslog_message() formatted into a 2048 byte buffer on its stack and
	then used vsnprintf()'s return value, the length the text would have had,
	to append the newline and as the size for write_port(): a byte of the
	caller's stack became '\n' (at a place the length picked), from about
	20,000 characters the program crashed, and the caller's stack was sent
	to the syslog daemon.

	Each length runs in a child that keeps a canary array on its stack above
	the syslog() call. Then the log is read back: every message must be a
	line of its own, cut at the buffer size.

	The last part writes a line of the largest size straight to the daemon's
	port, as the kernel does: the daemon copied such a line behind the header
	it puts in front, past the end of its buffer.
*/


#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <OS.h>

#include <KMessage.h>
#include <launch.h>
#include <syslog_daemon.h>


static const int kLengths[] = {
	100, 1000, 1980, 1990, 2000, 2010, 2020, 2047, 2048, 2100, 2500, 3000,
	4000, 6000, 20000, 60000
};
static const int kLengthCount = sizeof(kLengths) / sizeof(kLengths[0]);
static const char* const kLogPath = "/var/log/syslog";

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


static void
crashed(int signal)
{
	_exit(100 + signal);
}


static int __attribute__((noinline))
try_length(int length, int index)
{
	volatile unsigned char canary[4096];
	memset((void*)canary, 0x55, sizeof(canary));

	char* text = (char*)malloc(length + 1);
	memset(text, 'A' + index, length);
	text[length] = '\0';

	syslog(LOG_INFO, "%s", text);

	int changed = 0;
	for (size_t i = 0; i < sizeof(canary); i++) {
		if (canary[i] != 0x55) {
			printf("\tlength %d: the caller's stack byte %d became 0x%02x\n",
				length, (int)i, canary[i]);
			changed++;
		}
	}

	free(text);
	return changed;
}


/*!	Counts, in what was appended to the log since \a start, the lines that
	hold \a tag, and tells how long the longest is and how many of them hold
	a second message (the tag again, or \a other) or do not end.
*/
static int
count_lines(off_t start, const char* tag, const char* other, size_t* _longest,
	int* _glued)
{
	*_longest = 0;
	*_glued = 0;

	FILE* file = fopen(kLogPath, "r");
	if (file == NULL)
		return -1;
	fseeko(file, start, SEEK_SET);

	size_t size = 256 * 1024;
	char* line = (char*)malloc(size);
	int count = 0;
	while (fgets(line, size, file) != NULL) {
		char* found = strstr(line, tag);
		if (found == NULL)
			continue;
		count++;
		size_t length = strlen(line);
		if (length > *_longest)
			*_longest = length;
		if (strstr(found + 1, tag) != NULL || line[length - 1] != '\n'
			|| (other != NULL && strstr(line, other) != NULL)) {
			(*_glued)++;
		}
	}

	free(line);
	fclose(file);
	return count;
}


static off_t
log_size()
{
	FILE* file = fopen(kLogPath, "r");
	if (file == NULL)
		return -1;
	fseeko(file, 0, SEEK_END);
	off_t size = ftello(file);
	fclose(file);
	return size;
}


int
main()
{
	setvbuf(stdout, NULL, _IONBF, 0);

	char tag[B_OS_NAME_LENGTH];
	snprintf(tag, sizeof(tag), "syslog_long_%d", (int)getpid());

	off_t start = log_size();

	for (int i = 0; i < kLengthCount; i++) {
		pid_t child = fork();
		if (child < 0) {
			perror("fork");
			return 1;
		}
		if (child == 0) {
			signal(SIGSEGV, crashed);
			signal(SIGBUS, crashed);
			alarm(10);
			openlog(tag, 0, LOG_USER);
			_exit(try_length(kLengths[i], i) == 0 ? 0 : 1);
		}

		int status = 0;
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
		if (WIFSIGNALED(status)) {
			check(false, "length %d: the child died with signal %d",
				kLengths[i], WTERMSIG(status));
		} else if (WEXITSTATUS(status) >= 100) {
			check(false, "length %d: the child crashed with signal %d",
				kLengths[i], WEXITSTATUS(status) - 100);
		} else {
			check(WEXITSTATUS(status) == 0,
				"length %d: the caller's stack is intact", kLengths[i]);
		}
	}

	// What the daemon wrote: one line per message, none longer than
	// syslog()'s buffer allows.
	if (start < 0) {
		printf("skipped: %s cannot be read, the log is not checked\n",
			kLogPath);
	} else {
		size_t longest = 0;
		int glued = 0;
		int count = 0;
		for (int attempt = 0; attempt < 50; attempt++) {
			count = count_lines(start, tag, NULL, &longest, &glued);
			if (count >= kLengthCount)
				break;
			snooze(100000);
		}
		check(count == kLengthCount, "%d messages are %d lines in %s",
			kLengthCount, count, kLogPath);
		check(glued == 0, "%d lines hold more than one message or do not "
			"end", glued);
		check(longest <= 2048 + 128, "the longest line has %d characters",
			(int)longest);
	}

	// A line of the largest size, written to the daemon's port directly.
	BPrivate::KMessage data;
	port_id port = -1;
	if (BPrivate::get_launch_data(B_SYSTEM_LOGGER_SIGNATURE, data) == B_OK)
		port = data.GetInt32("logger_port", -1);
	if (port < 0) {
		printf("skipped: no syslog daemon port, the daemon is not checked\n");
	} else {
		static char buffer[SYSLOG_MESSAGE_BUFFER_SIZE];
		syslog_message& message = *(syslog_message*)buffer;
		message.from = find_thread(NULL);
		message.when = real_time_clock();
		message.options = LOG_PID;
		message.priority = LOG_USER | LOG_INFO;
		// an ident of the full size, for the longest header
		snprintf(tag, sizeof(tag), "syslog_long_port_%d_____________",
			(int)getpid());
		strlcpy(message.ident, tag, sizeof(message.ident));
		size_t length = SYSLOG_MAX_MESSAGE_LENGTH;
		memset(message.message, 'P', length);
		message.message[length - 1] = '\n';

		start = log_size();
		team_info before;
		port_info portInfo;
		get_port_info(port, &portInfo);
		get_team_info(portInfo.team, &before);

		status_t status = write_port_etc(port, SYSLOG_MESSAGE, buffer,
			sizeof(syslog_message) + length, B_RELATIVE_TIMEOUT, 2000000);
		check(status == B_OK, "a %d byte line written to the daemon's port: "
			"%s", (int)length, strerror(status));
		static const char* const kNext = "the message after the long line";
		syslog(LOG_INFO, "%s", kNext);

		size_t longest = 0;
		int glued = 0;
		int count = 0;
		for (int attempt = 0; attempt < 50; attempt++) {
			count = count_lines(start, tag, kNext, &longest, &glued);
			size_t unused;
			int unusedGlued;
			if (count >= 1 && count_lines(start, kNext, NULL, &unused,
					&unusedGlued) >= 1) {
				break;
			}
			snooze(100000);
		}
		check(count == 1, "the daemon wrote it as %d line", count);
		check(longest > 0 && longest <= SYSLOG_MESSAGE_BUFFER_SIZE,
			"the line has %d characters (the daemon's buffer has %d)",
			(int)longest, SYSLOG_MESSAGE_BUFFER_SIZE);
		check(glued == 0, "the line ends, the next message is a line of its "
			"own");

		team_info after;
		check(get_port_info(port, &portInfo) == B_OK
				&& get_team_info(portInfo.team, &after) == B_OK
				&& after.team == before.team,
			"the syslog daemon is still the same team");
	}

	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	syslog(LOG_INFO, "syslog_long_message_test: return sFailures == 0 ? 0 : 1;");
	return sFailures == 0 ? 0 : 1;
}
