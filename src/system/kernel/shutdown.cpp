/*
 * Copyright 2009, Olivier Coursière. All rights reserved.
 * Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <debug.h>
#include <kernel.h>
#include <syscalls.h>
#include <thread.h>


// Test only: implemented in fs/vfs_boot.cpp.
extern void dump_teams_for_diag(int32 round);


/*!	Test only: lists all teams with the object their threads wait on, from
	20 seconds after the shutdown started and then every 20 seconds, so a
	hang can be read from SYSLOG.TXT.
*/
static status_t
shutdown_diag_thread(void*)
{
	snooze(20000000);
	for (int32 round = 0; round < 9; round++) {
		dump_teams_for_diag(100 + round);
		snooze(20000000);
	}
	return B_OK;
}


status_t
system_shutdown(bool reboot)
{
	int32 cookie = 0;
	team_info info;

	// Test only: says where the shutdown is, for the case that it hangs.
	dprintf("shutdown: system_shutdown(%d) entered\n", reboot);
	thread_id diagThread = spawn_kernel_thread(&shutdown_diag_thread,
		"shutdown diag", B_LOW_PRIORITY, NULL);
	if (diagThread >= 0)
		resume_thread(diagThread);

	gKernelShutdown = true;
	dprintf("shutdown: gKernelShutdown set, %s\n",
		reboot ? "killing the teams" : "leaving the teams alone");

	// Now shutdown all system services!
	// TODO: Once we are sure we can shutdown the system on all hardware
	// checking reboot may not be necessary anymore.
	if (reboot) {
		while (get_next_team_info(&cookie, &info) == B_OK) {
			if (info.team == B_SYSTEM_TEAM)
				continue;
			kill_team(info.team);
		}
		dprintf("shutdown: teams killed\n");
	}

	dprintf("shutdown: syncing\n");
	sync();
	dprintf("shutdown: sync returned, calling arch_cpu_shutdown(%d)\n",
		reboot);

	return arch_cpu_shutdown(reboot);
}


//	#pragma mark -


status_t
_user_shutdown(bool reboot)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;
	return system_shutdown(reboot);
}
