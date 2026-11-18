/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Test tool, not for upstream: checks that writing to the eMMC works, on
	the Android partition "logs" only. It finds that partition by its GPT
	name, keeps a copy of it and of the first MiB of the disk, writes and
	reads back test patterns, measures the speed, and puts the old contents
	back. After every step it checks that the first MiB of the disk and the
	4 MiB on either side of the partition did not change; if the first MiB
	changed, it writes the copy back and stops.

	The report goes to ~/Desktop/emmc-write-test.txt and to the syslog.
*/


#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <Drivers.h>
#include <OS.h>


static const char* kPartitionName = "logs";
static const off_t kMaxPartitionSize = 64 * 1024 * 1024;
static const size_t kHeadSize = 1024 * 1024;
static const size_t kGuardSize = 4 * 1024 * 1024;
static const size_t kChunk = 1024 * 1024;

static FILE* sReport;


static void
report(const char* format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);

	printf("%s\n", line);
	if (sReport != NULL) {
		fprintf(sReport, "%s\n", line);
		fflush(sReport);
	}
	syslog(LOG_NOTICE, "emmc_write_test: %s", line);
}


static bool
read_all(int fd, off_t offset, void* buffer, size_t size)
{
	uint8* p = (uint8*)buffer;
	while (size > 0) {
		ssize_t n = pread(fd, p, size, offset);
		if (n <= 0) {
			report("read at %lld failed: %s", (long long)offset,
				strerror(n < 0 ? errno : EIO));
			return false;
		}
		p += n;
		offset += n;
		size -= n;
	}
	return true;
}


static bool
write_all(int fd, off_t offset, const void* buffer, size_t size)
{
	const uint8* p = (const uint8*)buffer;
	while (size > 0) {
		ssize_t n = pwrite(fd, p, size, offset);
		if (n <= 0) {
			report("write at %lld failed: %s", (long long)offset,
				strerror(n < 0 ? errno : EIO));
			return false;
		}
		p += n;
		offset += n;
		size -= n;
	}
	return true;
}


static void
flush(int fd)
{
	if (ioctl(fd, B_FLUSH_DRIVE_CACHE) != 0)
		fsync(fd);
}


static uint32
checksum(const uint8* data, size_t size)
{
	uint32 sum = 0x811c9dc5;
	for (size_t i = 0; i < size; i++)
		sum = (sum ^ data[i]) * 0x01000193;
	return sum;
}


static void
fill_pattern(uint8* buffer, size_t size, uint32 seed)
{
	uint32 x = seed * 2654435761u + 1;
	for (size_t i = 0; i < size; i += 4) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		memcpy(buffer + i, &x, 4);
	}
}


struct Disk {
	char	rawPath[B_PATH_NAME_LENGTH];
	char	partitionPath[B_PATH_NAME_LENGTH];
	off_t	offset;
	off_t	size;
	off_t	diskSize;
};


/*!	Finds a GPT entry named \a name on \a raw. */
static bool
find_gpt_entry(int raw, const char* name, off_t* offset, off_t* size)
{
	uint8 header[512];
	if (!read_all(raw, 512, header, sizeof(header)))
		return false;
	if (memcmp(header, "EFI PART", 8) != 0) {
		report("  no GPT");
		return false;
	}

	uint64 entriesLBA;
	uint32 count, entrySize;
	memcpy(&entriesLBA, header + 72, 8);
	memcpy(&count, header + 80, 4);
	memcpy(&entrySize, header + 84, 4);
	if (entrySize < 128 || entrySize > 512 || count > 256)
		return false;

	uint8* entries = (uint8*)malloc(count * entrySize);
	if (entries == NULL
		|| !read_all(raw, entriesLBA * 512, entries, count * entrySize)) {
		free(entries);
		return false;
	}

	bool found = false;
	for (uint32 i = 0; i < count; i++) {
		const uint8* entry = entries + i * entrySize;
		uint64 first, last;
		memcpy(&first, entry + 32, 8);
		memcpy(&last, entry + 40, 8);
		if (first == 0 && last == 0)
			continue;

		char entryName[37];
		for (int c = 0; c < 36; c++)
			entryName[c] = (char)entry[56 + c * 2];
		entryName[36] = '\0';

		report("  GPT %2u: %-16s LBA %llu to %llu (%llu MiB)", i, entryName,
			(unsigned long long)first, (unsigned long long)last,
			(unsigned long long)((last - first + 1) * 512 / 1048576));

		if (!found && strcmp(entryName, name) == 0) {
			*offset = first * 512;
			*size = (last - first + 1) * 512;
			found = true;
		}
	}
	free(entries);
	return found;
}


static bool
find_disk(Disk& disk)
{
	DIR* dir = opendir("/dev/disk/mmc");
	if (dir == NULL) {
		report("no /dev/disk/mmc");
		return false;
	}

	bool found = false;
	struct dirent* entry;
	while (!found && (entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		snprintf(disk.rawPath, sizeof(disk.rawPath), "/dev/disk/mmc/%s/raw",
			entry->d_name);
		int raw = open(disk.rawPath, O_RDONLY);
		if (raw < 0)
			continue;

		device_geometry geometry;
		if (ioctl(raw, B_GET_GEOMETRY, &geometry, sizeof(geometry)) == 0) {
			disk.diskSize = (off_t)geometry.bytes_per_sector
				* geometry.sectors_per_track * geometry.cylinder_count
				* geometry.head_count;
			report("%s: %lld MiB, %u byte sectors, read-only %d",
				disk.rawPath, (long long)(disk.diskSize / 1048576),
				geometry.bytes_per_sector, geometry.read_only);
			if (geometry.bytes_per_sector != 512 || geometry.read_only) {
				close(raw);
				continue;
			}
		}

		found = find_gpt_entry(raw, kPartitionName, &disk.offset, &disk.size);
		close(raw);
	}
	closedir(dir);
	if (!found) {
		report("no partition named \"%s\" found", kPartitionName);
		return false;
	}

	// The partition device whose offset matches the GPT entry
	char base[B_PATH_NAME_LENGTH];
	strlcpy(base, disk.rawPath, sizeof(base));
	*strrchr(base, '/') = '\0';
	for (int i = 0; i < 64; i++) {
		snprintf(disk.partitionPath, sizeof(disk.partitionPath), "%s/%d",
			base, i);
		int fd = open(disk.partitionPath, O_RDONLY);
		if (fd < 0)
			continue;
		partition_info info;
		bool match = ioctl(fd, B_GET_PARTITION_INFO, &info, sizeof(info)) == 0
			&& info.offset == disk.offset && info.size == disk.size;
		close(fd);
		if (match)
			return true;
	}
	report("no partition device at offset %lld", (long long)disk.offset);
	return false;
}


struct Guards {
	uint32	head;
	uint32	before;
	uint32	after;
};


static bool
read_guards(int raw, const Disk& disk, Guards& guards, uint8* buffer)
{
	if (!read_all(raw, 0, buffer, kHeadSize))
		return false;
	guards.head = checksum(buffer, kHeadSize);

	off_t before = disk.offset - kGuardSize;
	if (before < 0)
		before = 0;
	if (!read_all(raw, before, buffer, disk.offset - before))
		return false;
	guards.before = checksum(buffer, disk.offset - before);

	off_t after = disk.offset + disk.size;
	size_t afterSize = kGuardSize;
	if (after + (off_t)afterSize > disk.diskSize)
		afterSize = disk.diskSize - after;
	if (!read_all(raw, after, buffer, afterSize))
		return false;
	guards.after = checksum(buffer, afterSize);
	return true;
}


/*!	Checks that nothing outside the partition changed. If the first MiB
	(the GPT) changed, writes its copy back.
*/
static bool
check_guards(int raw, int rawWrite, const Disk& disk, const Guards& wanted,
	const uint8* headCopy, uint8* buffer, const char* step)
{
	Guards now;
	if (!read_guards(raw, disk, now, buffer))
		return false;
	bool ok = now.head == wanted.head && now.before == wanted.before
		&& now.after == wanted.after;
	report("%s: outside the partition %s (head %s, before %s, after %s)",
		step, ok ? "unchanged" : "CHANGED",
		now.head == wanted.head ? "ok" : "CHANGED",
		now.before == wanted.before ? "ok" : "CHANGED",
		now.after == wanted.after ? "ok" : "CHANGED");
	if (now.head != wanted.head) {
		report("writing the first MiB back");
		write_all(rawWrite, 0, headCopy, kHeadSize);
		flush(rawWrite);
	}
	return ok;
}


int
main()
{
	char reportPath[B_PATH_NAME_LENGTH];
	snprintf(reportPath, sizeof(reportPath), "%s/Desktop/emmc-write-test.txt",
		getenv("HOME") != NULL ? getenv("HOME") : "/boot/home");
	sReport = fopen(reportPath, "w");

	report("eMMC write test, on the partition \"%s\" only", kPartitionName);

	Disk disk;
	memset(&disk, 0, sizeof(disk));
	if (!find_disk(disk))
		return 1;
	report("partition %s: offset %lld, %lld MiB", disk.partitionPath,
		(long long)disk.offset, (long long)(disk.size / 1048576));
	if (disk.size > kMaxPartitionSize || disk.size % kChunk != 0
		|| disk.diskSize < disk.offset + disk.size) {
		report("unexpected size, stopping");
		return 1;
	}

	int raw = open(disk.rawPath, O_RDONLY);
	int rawWrite = open(disk.rawPath, O_RDWR);
	int part = open(disk.partitionPath, O_RDWR);
	if (raw < 0 || rawWrite < 0 || part < 0) {
		report("cannot open the devices: %s", strerror(errno));
		return 1;
	}

	size_t size = disk.size;
	uint8* backup = (uint8*)malloc(size);
	uint8* pattern = (uint8*)malloc(size);
	uint8* check = (uint8*)malloc(size > kGuardSize ? size : kGuardSize);
	uint8* headCopy = (uint8*)malloc(kHeadSize);
	if (backup == NULL || pattern == NULL || check == NULL
		|| headCopy == NULL) {
		report("out of memory");
		return 1;
	}

	// Copies first
	if (!read_all(raw, 0, headCopy, kHeadSize)
		|| !read_all(part, 0, backup, size))
		return 1;
	Guards guards;
	if (!read_guards(raw, disk, guards, check))
		return 1;
	FILE* file = fopen("/boot/home/emmc-test-head.bin", "wb");
	if (file != NULL) {
		fwrite(headCopy, 1, kHeadSize, file);
		fclose(file);
	}
	file = fopen("/boot/home/emmc-test-logs.bin", "wb");
	if (file != NULL) {
		fwrite(backup, 1, size, file);
		fclose(file);
	}
	report("copies kept in /boot/home/emmc-test-head.bin and "
		"emmc-test-logs.bin (checksum %08x)", checksum(backup, size));

	bool ok = true;

	// 1. One sector
	fill_pattern(pattern, 512, 1);
	if (write_all(part, 0, pattern, 512)) {
		flush(part);
		ok &= read_all(part, 0, check, 512) && memcmp(check, pattern, 512) == 0;
	} else
		ok = false;
	report("1 sector: %s", ok ? "ok" : "FAILED");
	if (!check_guards(raw, rawWrite, disk, guards, headCopy, check,
			"after 1 sector"))
		ok = false;

	// 2. The whole partition, 1 MiB at a time, timed
	if (ok) {
		fill_pattern(pattern, size, 2);
		bigtime_t start = system_time();
		for (size_t offset = 0; ok && offset < size; offset += kChunk)
			ok = write_all(part, offset, pattern + offset, kChunk);
		flush(part);
		bigtime_t writeTime = system_time() - start;

		start = system_time();
		for (size_t offset = 0; ok && offset < size; offset += kChunk)
			ok = read_all(part, offset, check + offset, kChunk);
		bigtime_t readTime = system_time() - start;

		size_t bad = 0;
		for (size_t offset = 0; ok && offset < size; offset += 512) {
			if (memcmp(check + offset, pattern + offset, 512) != 0) {
				if (bad < 5)
					report("  sector %zu differs", offset / 512);
				bad++;
			}
		}
		if (bad != 0)
			ok = false;
		report("whole partition: %s; write %.1f MB/s, read %.1f MB/s, "
			"%zu bad sectors", ok ? "ok" : "FAILED",
			size / (double)writeTime, size / (double)readTime, bad);
		if (!check_guards(raw, rawWrite, disk, guards, headCopy, check,
				"after the whole partition"))
			ok = false;
	}

	// 3. 300 small writes at random places, 512 bytes to 64 KiB
	if (ok) {
		srand(3);
		int bad = 0;
		for (int i = 0; ok && i < 300; i++) {
			size_t length = 512 << (rand() % 8);
			off_t offset = ((off_t)rand() % (size / 512)) * 512;
			if (offset + (off_t)length > (off_t)size)
				offset = size - length;
			fill_pattern(pattern + offset, length, 100 + i);
			ok = write_all(part, offset, pattern + offset, length);
		}
		flush(part);
		ok = ok && read_all(part, 0, check, size);
		for (size_t offset = 0; ok && offset < size; offset += 512) {
			if (memcmp(check + offset, pattern + offset, 512) != 0)
				bad++;
		}
		if (bad != 0)
			ok = false;
		report("300 small writes: %s, %d bad sectors", ok ? "ok" : "FAILED",
			bad);
		if (!check_guards(raw, rawWrite, disk, guards, headCopy, check,
				"after the small writes"))
			ok = false;
	}

	// 4. Put the old contents back, always
	bool restored = write_all(part, 0, backup, size);
	flush(part);
	restored = restored && read_all(part, 0, check, size)
		&& memcmp(check, backup, size) == 0;
	report("old contents back: %s", restored ? "ok" : "FAILED");
	check_guards(raw, rawWrite, disk, guards, headCopy, check, "at the end");

	report("RESULT: %s", ok && restored ? "PASS" : "FAIL");

	close(part);
	close(rawWrite);
	close(raw);
	if (sReport != NULL)
		fclose(sReport);
	return ok && restored ? 0 : 1;
}
