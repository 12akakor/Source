/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	A library with TLS is loaded, unloaded and loaded again on the main
	thread. A new thread then sets its own TLS value and reads it back. With
	the unfixed runtime_loader the thread's TLS generation starts at 0, so
	its freshly created block is treated as stale and discarded: the value
	reads back as 0 (#HK-53 from haiku-pi/haiku-rpi).
*/


#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>


typedef int (*set_value_t)(int);
typedef int (*get_value_t)();


static set_value_t sSet = NULL;
static get_value_t sGet = NULL;
static int sResult = -1;


static void*
thread_func(void*)
{
	if (sSet == NULL || sGet == NULL)
		return NULL;

	sSet(1234);
	sResult = sGet();
	return NULL;
}


int
main()
{
	const char* path = "/boot/home/tests/libtls_reload.so";

	for (int round = 0; round < 3; round++) {
		void* handle = dlopen(path, RTLD_NOW);
		if (handle == NULL) {
			printf("FAILED: dlopen: %s\n", dlerror());
			syslog(LOG_INFO, "tls_reload_test: FAILED dlopen: %s", dlerror());
			return 1;
		}
		sSet = (set_value_t)dlsym(handle, "set_tls_value");
		sGet = (get_value_t)dlsym(handle, "get_tls_value");
		if (sSet == NULL || sGet == NULL) {
			printf("FAILED: dlsym\n");
			syslog(LOG_INFO, "tls_reload_test: FAILED dlsym");
			return 1;
		}
		if (round < 2) {
			sSet(7);
			dlclose(handle);
		}
	}

	pthread_t thread;
	pthread_create(&thread, NULL, thread_func, NULL);
	pthread_join(thread, NULL);

	const char* result = sResult == 1234 ? "ok" : "FAILED";
	printf("RESULT: %s, new thread's TLS value %d (expected 1234)\n", result,
		sResult);
	syslog(LOG_INFO, "tls_reload_test: RESULT: %s (value %d)", result, sResult);
	return sResult == 1234 ? 0 : 1;
}
