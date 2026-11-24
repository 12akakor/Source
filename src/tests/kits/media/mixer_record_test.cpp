/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Smoke test for the mixer's auto-stop path. A sound card node stays
	running while something records from it: stopping it when playback ends
	ends the recording early (#151). This test checks that the media roster
	is reachable; the actual playback/recording sequence needs a sound card
	and cannot be automated here.
*/


#include <stdio.h>
#include <syslog.h>

#include <MediaRoster.h>


int
main()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	const char* result = roster != NULL
		? "ok, media roster reachable" : "FAILED, no media roster";
	printf("RESULT: %s\n", result);
	syslog(LOG_INFO, "mixer_record_test: RESULT: %s", result);
	return roster != NULL ? 0 : 1;
}
