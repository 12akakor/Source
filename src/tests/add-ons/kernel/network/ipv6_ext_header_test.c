/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Sends an IPv6 packet with a single hop-by-hop extension header whose
	length field is 0. IPv6Header::GetHeaderOffset() used to add that field
	to the offset instead of the header's real 8-byte size, so the loop read
	the same header again and again: the receive path never returned.
	Run as root: the fix also restricts raw sockets to root.
*/


#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include <OS.h>


static volatile bool sDone;


static status_t
watchdog(void*)
{
	snooze(5000000);
	if (!sDone) {
		const char* message
			= "ipv6_ext_header_test: FAILED, the receive path hung";
		puts(message);
		syslog(LOG_ERR, "%s", message);
		_exit(1);
	}
	return B_OK;
}


int
main()
{
	if (geteuid() != 0) {
		puts("ipv6_ext_header_test: run as root (su), raw sockets are "
			"restricted");
		return 2;
	}

	int fd = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
	if (fd < 0) {
		printf("ipv6_ext_header_test: socket: %s\n", strerror(errno));
		return 2;
	}

	thread_id watchdogThread = spawn_thread(watchdog, "watchdog",
		B_NORMAL_PRIORITY, NULL);
	resume_thread(watchdogThread);

	uint8 packet[64];
	memset(packet, 0, sizeof(packet));
	packet[0] = 0x60;					// version 6
	packet[4] = 0; packet[5] = 8;		// payload length: one header
	packet[6] = IPPROTO_HOPOPTS;		// next header
	packet[7] = 64;						// hop limit
	packet[8 + 24 + 15] = 1;			// destination ::1
	packet[40] = IPPROTO_HOPOPTS;		// extension header: next
	packet[41] = 0;						// length 0 == 8 bytes total

	struct sockaddr_in6 destination;
	memset(&destination, 0, sizeof(destination));
	destination.sin6_family = AF_INET6;
	destination.sin6_addr.s6_addr[15] = 1;

	puts("ipv6_ext_header_test: sending malformed extension header to ::1");
	syslog(LOG_INFO, "ipv6_ext_header_test: sending malformed extension header");
	fflush(stdout);

	ssize_t sent = sendto(fd, packet, 48, 0,
		(struct sockaddr*)&destination, sizeof(destination));
	printf("ipv6_ext_header_test: sendto returned %zd\n", sent);
	close(fd);

	sDone = true;
	const char* result = sent == 48
		? "ok, the stack dropped the malformed packet" : "send failed";
	printf("RESULT: %s\n", result);
	syslog(LOG_INFO, "ipv6_ext_header_test: RESULT: %s", result);
	return sent == 48 ? 0 : 1;
}
