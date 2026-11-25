/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	msync(MS_SYNC) of a mapping that lies 4 GiB or more into its file
	(haiku#183, item 3).

	_user_sync_memory() kept the offset into the file's cache in 32 bits:
	for such a mapping the pages at the offset modulo 4 GiB were written
	instead, msync() returned success, and the data stayed in memory until
	the page writer got to it.

	Whether a page has reached the disk cannot be seen through the file. So
	the file is put on a BFS volume in an image file, and the image file is
	read: a page written through the mapping must be in it when msync() has
	returned. A mapping below 4 GiB is the control.

	Usage: msync_large_offset_test <directory>
	The directory's volume needs 4.4 GiB of free space; creating the two
	large files takes a minute or two on BFS.
*/


#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>
#include <fs_volume.h>


static const off_t kGiB = 1024LL * 1024 * 1024;
static const off_t kLargeOffset = 4 * kGiB + 1024 * 1024;
static const off_t kSmallOffset = 16 * 1024 * 1024;
static const off_t kFileSize = kLargeOffset + 2 * B_PAGE_SIZE;
static const off_t kImageSize = kFileSize + 128 * 1024 * 1024;

static int sFailures;
static char sImagePath[B_PATH_NAME_LENGTH];
static char sMountPoint[B_PATH_NAME_LENGTH];
static bool sMounted;


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
clean_up()
{
	if (sMounted)
		fs_unmount_volume(sMountPoint, B_FORCE_UNMOUNT);
	sMounted = false;
	rmdir(sMountPoint);
	unlink(sImagePath);
}


static void
fatal(const char* what)
{
	printf("FAILED: %s: %s\n", what, strerror(errno));
	clean_up();
	exit(1);
}


static void
fill_page(char* page, char kind, off_t offset)
{
	// a pattern that occurs nowhere else in the image
	for (size_t i = 0; i < B_PAGE_SIZE; i += 32) {
		snprintf(page + i, 32, "msync-%c-%012llx-%05x......", kind,
			(long long)offset, (unsigned)i);
		page[i + 31] = '\n';
	}
}


/*!	Finds \a page in the image and returns its offset there, or -1. The
	volume is new and the file its only one, so its blocks follow each other:
	the search starts near the same offset.
*/
static off_t
find_in_image(int image, const char* page, off_t expected)
{
	const size_t kChunk = 4 * 1024 * 1024;
	char* buffer = (char*)malloc(kChunk);
	off_t found = -1;

	for (int pass = 0; pass < 2 && found < 0; pass++) {
		off_t start = pass == 0 ? expected & ~(off_t)(kChunk - 1) : 0;
		off_t end = pass == 0 ? kImageSize : expected;
		for (off_t offset = start; offset < end && found < 0;
				offset += kChunk) {
			ssize_t bytesRead = pread(image, buffer, kChunk, offset);
			if (bytesRead <= 0)
				break;
			for (ssize_t i = 0; i + (ssize_t)B_PAGE_SIZE <= bytesRead;
					i += 512) {
				if (memcmp(buffer + i, page, B_PAGE_SIZE) == 0) {
					found = offset + i;
					break;
				}
			}
		}
	}

	free(buffer);
	return found;
}


static void
test_offset(int image, int file, off_t offset, const char* name)
{
	char first[B_PAGE_SIZE];
	char second[B_PAGE_SIZE];
	char inImage[B_PAGE_SIZE];
	fill_page(first, 'A', offset);
	fill_page(second, 'B', offset);

	// Write a page the ordinary way and find its place in the image.
	if (pwrite(file, first, B_PAGE_SIZE, offset) != (ssize_t)B_PAGE_SIZE
		|| fsync(file) != 0) {
		check(false, "%s: write and fsync: %s", name, strerror(errno));
		return;
	}
	off_t place = find_in_image(image, first, offset);
	if (place < 0) {
		check(false, "%s: the page written with write() and fsync() is not "
			"in the image", name);
		return;
	}

	// Change it through a mapping and sync that.
	char* mapping = (char*)mmap(NULL, B_PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_SHARED, file, offset);
	if (mapping == MAP_FAILED) {
		check(false, "%s: mmap: %s", name, strerror(errno));
		return;
	}
	memcpy(mapping, second, B_PAGE_SIZE);
	int result = msync(mapping, B_PAGE_SIZE, MS_SYNC);
	int error = errno;

	bool onDisk = pread(image, inImage, B_PAGE_SIZE, place)
			== (ssize_t)B_PAGE_SIZE
		&& memcmp(inImage, second, B_PAGE_SIZE) == 0;
	check(result == 0, "%s: msync(MS_SYNC): %s", name,
		result == 0 ? "no error" : strerror(error));
	check(onDisk, "%s: the page changed through the mapping is %s the image "
		"after msync() (at %lld there)", name, onDisk ? "in" : "NOT in",
		(long long)place);

	munmap(mapping, B_PAGE_SIZE);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc != 2) {
		fprintf(stderr, "usage: %s <directory on a volume with 4.4 GiB "
			"free>\n", argv[0]);
		return 2;
	}
	alarm(900);

	snprintf(sImagePath, sizeof(sImagePath), "%s/msync_test_%d.image",
		argv[1], (int)getpid());
	snprintf(sMountPoint, sizeof(sMountPoint), "%s/msync_test_%d.mount",
		argv[1], (int)getpid());

	bigtime_t start = system_time();
	int image = open(sImagePath, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (image < 0 || ftruncate(image, kImageSize) != 0)
		fatal("creating the image file");
	printf("image file of %lld MiB created in %.1f s\n",
		(long long)(kImageSize >> 20), (system_time() - start) / 1e6);

	char command[2 * B_PATH_NAME_LENGTH];
	snprintf(command, sizeof(command), "mkfs -q -t bfs '%s' msync_test "
		">/dev/null 2>&1", sImagePath);
	if (system(command) != 0) {
		errno = EIO;
		fatal("mkfs of the image file");
	}
	if (mkdir(sMountPoint, 0755) != 0)
		fatal("creating the mount point");
	dev_t volume = fs_mount_volume(sMountPoint, sImagePath, "bfs", 0, NULL);
	if (volume < 0) {
		errno = volume;
		fatal("mounting the image file");
	}
	sMounted = true;

	char filePath[B_PATH_NAME_LENGTH + 16];
	snprintf(filePath, sizeof(filePath), "%s/large", sMountPoint);
	start = system_time();
	int file = open(filePath, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (file < 0 || ftruncate(file, kFileSize) != 0)
		fatal("creating the large file");
	printf("file of %lld MiB created in %.1f s\n",
		(long long)(kFileSize >> 20), (system_time() - start) / 1e6);

	test_offset(image, file, kSmallOffset, "16 MiB into the file");
	test_offset(image, file, kLargeOffset, "4 GiB + 1 MiB into the file");

	close(file);
	close(image);
	clean_up();

	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	syslog(LOG_INFO, "msync_large_offset_test: return sFailures == 0 ? 0 : 1;");
	return sFailures == 0 ? 0 : 1;
}
