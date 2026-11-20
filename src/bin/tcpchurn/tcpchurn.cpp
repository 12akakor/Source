/*
 * Copyright 2026, haiku-odroid-c2 port authors.
 * Distributed under the terms of the MIT License.
 */

/*!	Closes TCP connections while the peer is still sending, so a socket's last
	reference goes in the program's thread while the network's receive thread
	is looking up that same connection.

		tcpchurn server <port> <threads> <max hold in us> <seconds> [fin]
		tcpchurn client <host> <port> <threads> <seconds> [reset]

	The server reads for a random time up to the hold, then closes with a reset
	(SO_LINGER, 0). The client sends until that reset arrives, and connects again.
	With "fin", the server closes the ordinary way, so that the endpoints stay for
	their time wait and the timer or a reset ends them.
	With "reset", the client itself resets straight after connecting, which
	races the server's accept against the abort of the connection being accepted.
	Each prints its count of connections every 10 seconds and at the end.
*/

#include <OS.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


static int32 sCount;
static int sListener;
static long sMaxHold;
static const char* sHost;
static const char* sPort;
static int sReset;
static int sFin;


static int64_t
now_us()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}


static void*
server_thread(void* cookie)
{
	uint32_t random = (uint32_t)(uintptr_t)cookie * 2654435761u + 1;
	static char buffer[65536];

	while (1) {
		int fd = accept(sListener, NULL, NULL);
		if (fd < 0) {
			usleep(1000);
			continue;
		}

		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		int64_t end = now_us() + random % (uint32_t)sMaxHold;
		while (now_us() < end) {
			ssize_t bytes = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
			if (bytes == 0
				|| (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
				break;
			}
			if (bytes < 0)
				usleep(50);
		}

		if (!sFin) {
			struct linger linger;
			linger.l_onoff = 1;
			linger.l_linger = 0;
			setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
		}
		close(fd);
		atomic_add(&sCount, 1);
	}
	return NULL;
}


static void*
client_thread(void* cookie)
{
	static char buffer[65536];
	struct addrinfo hints;
	struct addrinfo* info;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(sHost, sPort, &hints, &info) != 0) {
		fprintf(stderr, "tcpchurn: can't resolve %s\n", sHost);
		exit(1);
	}

	while (1) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			usleep(1000);
			continue;
		}
		struct timeval timeout;
		timeout.tv_sec = 2;
		timeout.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		if (connect(fd, info->ai_addr, info->ai_addrlen) != 0) {
			close(fd);
			usleep(1000);
			continue;
		}
		if (sReset) {
			struct linger linger;
			linger.l_onoff = 1;
			linger.l_linger = 0;
			setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
		} else {
			while (send(fd, buffer, sizeof(buffer), MSG_NOSIGNAL) > 0)
				;
		}
		close(fd);
		atomic_add(&sCount, 1);
	}
	return NULL;
}


int
main(int argc, char** argv)
{
	int threads;
	int seconds;
	void* (*function)(void*);

	signal(SIGPIPE, SIG_IGN);

	if ((argc == 6 || argc == 7) && strcmp(argv[1], "server") == 0) {
		sFin = argc == 7;
		threads = atoi(argv[3]);
		sMaxHold = atol(argv[4]);
		seconds = atoi(argv[5]);
		function = server_thread;

		sListener = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(sListener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		struct sockaddr_in address;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
#ifdef __HAIKU__
		address.sin_len = sizeof(address);
#endif
		address.sin_port = htons(atoi(argv[2]));
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(sListener, (struct sockaddr*)&address, sizeof(address)) != 0
			|| listen(sListener, 128) != 0) {
			perror("tcpchurn: bind/listen");
			return 1;
		}
	} else if ((argc == 6 || argc == 7) && strcmp(argv[1], "client") == 0) {
		sReset = argc == 7;
		sHost = argv[2];
		sPort = argv[3];
		threads = atoi(argv[4]);
		seconds = atoi(argv[5]);
		function = client_thread;
	} else {
		fprintf(stderr, "usage: tcpchurn server <port> <threads> <max hold in us> "
			"<seconds> [fin]\n       tcpchurn client <host> <port> <threads> <seconds> "
			"[reset]\n");
		return 1;
	}

	for (int i = 0; i < threads; i++) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, function, (void*)(uintptr_t)(i + 1)) != 0) {
			perror("tcpchurn: pthread_create");
			return 1;
		}
	}

	int64_t start = now_us();
	for (int elapsed = 0; elapsed < seconds; elapsed += 10) {
		sleep(10);
		printf("%d s: %ld connections\n", (int)((now_us() - start) / 1000000), (long)atomic_get(&sCount));
		fflush(stdout);
	}
	_exit(0);
}
