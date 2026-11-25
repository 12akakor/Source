/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	semctl() with a semaphore number outside the set (haiku#183, item 1).

	The kernel accepted the set's size as a semaphore number: GETVAL, GETPID,
	GETNCNT and GETZCNT read what lies behind the set's array of semaphores,
	and SETVAL wrote there. The commands that do not use the number (IPC_STAT,
	IPC_SET, GETALL, SETALL, IPC_RMID) ignore it, as POSIX has it.
*/


#include <syslog.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/stat.h>


union semun {
	int					val;
	struct semid_ds*	buf;
	unsigned short*		array;
};

static const int kSemaphoreCount = 3;

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
expect_invalid(int id, int number, int command, const char* name)
{
	semun argument;
	argument.val = 1;
	errno = 0;
	int result = semctl(id, number, command, argument);
	check(result == -1 && errno == EINVAL, "semctl(set of %d, semaphore %d, "
		"%s) = %d, %s", kSemaphoreCount, number, name, result,
		errno != 0 ? strerror(errno) : "no error");
}


int
main()
{
	int id = semget(IPC_PRIVATE, kSemaphoreCount, IPC_CREAT | S_IRUSR
		| S_IWUSR);
	if (id < 0) {
		printf("FAILED: semget: %s\n", strerror(errno));
		return 1;
	}

	semun argument;
	for (int i = 0; i < kSemaphoreCount; i++) {
		argument.val = 10 + i;
		int result = semctl(id, i, SETVAL, argument);
		check(result == 0, "semctl(semaphore %d, SETVAL %d) = %d", i,
			argument.val, result);
		result = semctl(id, i, GETVAL, argument);
		check(result == 10 + i, "semctl(semaphore %d, GETVAL) = %d", i,
			result);
	}

	static const struct {
		int			command;
		const char*	name;
	} kCommands[] = {
		{ GETVAL, "GETVAL" },
		{ SETVAL, "SETVAL" },
		{ GETPID, "GETPID" },
		{ GETNCNT, "GETNCNT" },
		{ GETZCNT, "GETZCNT" }
	};
	for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
		expect_invalid(id, kSemaphoreCount, kCommands[i].command,
			kCommands[i].name);
		expect_invalid(id, kSemaphoreCount + 1, kCommands[i].command,
			kCommands[i].name);
		expect_invalid(id, -1, kCommands[i].command, kCommands[i].name);
	}

	// the values are still what was set
	unsigned short values[kSemaphoreCount];
	argument.array = values;
	int result = semctl(id, kSemaphoreCount, GETALL, argument);
	check(result == 0 && values[0] == 10 && values[1] == 11
			&& values[2] == 12,
		"semctl(semaphore %d, GETALL) = %d: %d %d %d (the number is "
		"ignored)", kSemaphoreCount, result, values[0], values[1], values[2]);

	struct semid_ds info;
	argument.buf = &info;
	result = semctl(id, 99, IPC_STAT, argument);
	check(result == 0 && info.sem_nsems == kSemaphoreCount,
		"semctl(semaphore 99, IPC_STAT) = %d (the number is ignored)",
		result);

	result = semctl(id, kSemaphoreCount, IPC_RMID, argument);
	check(result == 0, "semctl(semaphore %d, IPC_RMID) = %d (the number is "
		"ignored)", kSemaphoreCount, result);
	if (result != 0)
		semctl(id, 0, IPC_RMID, argument);

	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	syslog(LOG_INFO, "xsi_sem_number_test: return sFailures == 0 ? 0 : 1;");
	return sFailures == 0 ? 0 : 1;
}
