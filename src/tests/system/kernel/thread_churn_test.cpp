/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Control test: many threads start and exit, without affinity changes
	and without NVMM. If this panics in thread teardown, the panic is a
	pre-existing race and not caused by the imported fixes.
*/


#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

#include <OS.h>


static status_t
worker(void*)
{
	snooze(1000);
	return B_OK;
}


int
main()
{
	for (int32 round = 0; round < 20; round++) {
		thread_id threads[8];
		for (int32 i = 0; i < 8; i++) {
			threads[i] = spawn_thread(worker, "churn", B_NORMAL_PRIORITY,
				NULL);
			resume_thread(threads[i]);
		}
		for (int32 i = 0; i < 8; i++)
			wait_for_thread(threads[i], NULL);
	}

	const char* result = "ok, 160 threads spawned and joined";
	printf("RESULT: %s\n", result);
	syslog(LOG_INFO, "thread_churn_test: RESULT: %s", result);
	return 0;
}
