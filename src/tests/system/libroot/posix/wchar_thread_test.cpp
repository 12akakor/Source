/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	mbstowcs() and wcstombs() from several threads at once. Both use one
	static mbstate_t, which with the ICU locale backend holds a converter
	that two threads must not use at the same time. Without the lock this
	test reports wrong conversions or crashes in libroot.
*/


#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <wchar.h>


static const char* kASCII = "the quick brown fox jumps over the lazy dog";
static int sFailures = 0;
static pthread_mutex_t sFailureLock = PTHREAD_MUTEX_INITIALIZER;


static void*
convert(void* data)
{
	long id = (long)data;
	for (int round = 0; round < 2000; round++) {
		wchar_t wide[128];
		char narrow[128];
		size_t w = mbstowcs(wide, kASCII, 128);
		size_t n = wcstombs(narrow, wide, 128);
		if (w != strlen(kASCII) || n != w || memcmp(narrow, kASCII, n) != 0) {
			pthread_mutex_lock(&sFailureLock);
			if (sFailures < 5)
				printf("thread %ld round %d: w=%zu n=%zu\n", id, round, w, n);
			sFailures++;
			pthread_mutex_unlock(&sFailureLock);
		}
	}
	return NULL;
}


int
main()
{
	pthread_t threads[8];
	for (long i = 0; i < 8; i++)
		pthread_create(&threads[i], NULL, convert, (void*)i);
	for (int i = 0; i < 8; i++)
		pthread_join(threads[i], NULL);

	const char* result = sFailures == 0
		? "ok, 8 threads x 2000 mbstowcs/wcstombs rounds" : "FAILED";
	printf("RESULT: %s (%d bad conversions)\n", result, sFailures);
	syslog(LOG_INFO, "wchar_thread_test: RESULT: %s (%d bad conversions)",
		result, sFailures);
	return sFailures == 0 ? 0 : 1;
}
