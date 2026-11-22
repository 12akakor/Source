/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudJournal.h"

#include "PCloudCrypto.h"
#include "PCloudLogger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <new>


static const uint32 kJournalMagic = 'PCJ1';

struct JournalHeader {
	uint32 magic;
	uint32 type;
	uint32 length;
	uint32 crc;
};


PCloudJournal::PCloudJournal()
	:
	fPath()
{
}


PCloudJournal::~PCloudJournal()
{
}


status_t
PCloudJournal::Init(const BString& path)
{
	fPath = path;
	int fd = open(fPath.String(), O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0)
		return (status_t)errno;
	close(fd);
	return B_OK;
}


status_t
PCloudJournal::Append(uint32 type, const BMessage& payload)
{
	ssize_t flatSize = payload.FlattenedSize();
	if (flatSize <= 0 || flatSize > 16 * 1024 * 1024)
		return B_BAD_VALUE;

	char* buffer = new(std::nothrow) char[flatSize];
	if (buffer == NULL)
		return B_NO_MEMORY;
	status_t status = payload.Flatten(buffer, flatSize);
	if (status != B_OK) {
		delete[] buffer;
		return status;
	}

	JournalHeader header;
	header.magic = kJournalMagic;
	header.type = type;
	header.length = (uint32)flatSize;
	header.crc = PCloudCrypto::Crc32C(buffer, flatSize);

	int fd = open(fPath.String(), O_WRONLY | O_APPEND);
	if (fd < 0) {
		delete[] buffer;
		return (status_t)errno;
	}

	char* record = new(std::nothrow) char[sizeof(header) + flatSize];
	if (record == NULL) {
		delete[] buffer;
		close(fd);
		return B_NO_MEMORY;
	}
	memcpy(record, &header, sizeof(header));
	memcpy(record + sizeof(header), buffer, flatSize);

	size_t remaining = sizeof(header) + flatSize;
	const char* data = record;
	while (remaining > 0) {
		ssize_t written = write(fd, data, remaining);
		if (written <= 0) {
			status = written < 0 ? (status_t)errno : B_IO_ERROR;
			break;
		}
		data += written;
		remaining -= written;
	}
	if (status == B_OK && fsync(fd) != 0)
		status = (status_t)errno;

	delete[] record;
	delete[] buffer;
	close(fd);
	return status;
}


status_t
PCloudJournal::Replay(BObjectList<Record>& records)
{
	int fd = open(fPath.String(), O_RDONLY);
	if (fd < 0)
		return (status_t)errno;

	struct stat st;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return (status_t)errno;
	}
	off_t fileSize = st.st_size;

	off_t position = 0;
	status_t result = B_OK;
	while (position + (off_t)sizeof(JournalHeader) <= fileSize) {
		JournalHeader header;
		if (pread(fd, &header, sizeof(header), position)
				!= (ssize_t)sizeof(header)) {
			result = B_IO_ERROR;
			break;
		}
		if (header.magic != kJournalMagic || header.length == 0
			|| header.length > 16 * 1024 * 1024
			|| position + sizeof(header) + header.length > (uint32)fileSize) {
			PCLOUD_LOG_WARNING("journal: torn tail at offset %" B_PRIdOFF,
				position);
			break;
		}

		char* buffer = new(std::nothrow) char[header.length];
		if (buffer == NULL) {
			result = B_NO_MEMORY;
			break;
		}
		ssize_t bytes = pread(fd, buffer, header.length,
			position + sizeof(header));
		if (bytes != (ssize_t)header.length
			|| PCloudCrypto::Crc32C(buffer, header.length) != header.crc) {
			PCLOUD_LOG_WARNING("journal: checksum mismatch at offset %"
				B_PRIdOFF, position);
			delete[] buffer;
			break;
		}

		BMessage payload;
		result = payload.Unflatten(buffer);
		delete[] buffer;
		if (result != B_OK)
			break;

		Record* record = new(std::nothrow) Record;
		if (record == NULL) {
			result = B_NO_MEMORY;
			break;
		}
		record->type = header.type;
		record->payload = payload;
		if (!records.AddItem(record)) {
			delete record;
			result = B_NO_MEMORY;
			break;
		}
		position += sizeof(header) + header.length;
	}

	close(fd);
	if (position < fileSize) {
		PCLOUD_LOG_WARNING("journal: ignoring %" B_PRIdOFF
			" bytes of incomplete tail", fileSize - position);
	}
	return result;
}


status_t
PCloudJournal::Clear()
{
	int fd = open(fPath.String(), O_WRONLY | O_TRUNC);
	if (fd < 0)
		return (status_t)errno;
	status_t status = fsync(fd) == 0 ? B_OK : (status_t)errno;
	close(fd);
	return status;
}
