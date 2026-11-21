/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudCache.h"

#include "PCloudCrypto.h"
#include "PCloudDefs.h"
#include "PCloudLogger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <new>

#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>


static status_t
sync_directory(const BString& path)
{
	int fd = open(path.String(), O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		return (status_t)errno;
	int result = fsync(fd);
	close(fd);
	return result == 0 ? B_OK : (status_t)errno;
}


static status_t
flatten_message(const BMessage& message, char*& buffer, ssize_t& size)
{
	size = message.FlattenedSize();
	if (size <= 0)
		return B_BAD_VALUE;
	buffer = new(std::nothrow) char[size];
	if (buffer == NULL)
		return B_NO_MEMORY;
	return message.Flatten(buffer, size);
}


PCloudCache::PCloudCache()
	:
	fRoot()
{
}


status_t
PCloudCache::Init(const BString& accountTag)
{
	BPath path;
	status_t status = find_directory(B_USER_CACHE_DIRECTORY, &path);
	if (status != B_OK)
		return status;
	if (path.Append("pcloudfs") != B_OK)
		return B_ERROR;
	if (accountTag.Length() > 0 && path.Append(accountTag.String()) != B_OK)
		return B_ERROR;
	fRoot = path.Path();
	return _EnsureDirectories();
}


status_t
PCloudCache::_EnsureDirectories()
{
	const char* kDirectories[] = { "meta", "children", "data" };
	for (size_t i = 0; i < sizeof(kDirectories) / sizeof(kDirectories[0]);
		i++) {
		BPath path(fRoot.String());
		if (path.Append(kDirectories[i]) != B_OK)
			return B_ERROR;
		BDirectory directory;
		status_t status = directory.SetTo(path.Path());
		if (status == B_ENTRY_NOT_FOUND) {
			status = create_directory(path.Path(), 0700);
			if (status == B_OK)
				status = directory.SetTo(path.Path());
		}
		if (status != B_OK)
			return status;
	}
	return B_OK;
}


status_t
PCloudCache::_WriteAtomic(const BString& path, const void* data, size_t size)
{
	BString temporary;
	temporary.SetToFormat("%s.tmp.%ld", path.String(), (long)getpid());

	int fd = open(temporary.String(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return (status_t)errno;
	const char* bytes = static_cast<const char*>(data);
	size_t remaining = size;
	while (remaining > 0) {
		ssize_t written = write(fd, bytes, remaining);
		if (written <= 0) {
			int error = errno;
			close(fd);
			BEntry failed(temporary.String());
			failed.Remove();
			return written < 0 ? (status_t)error : B_IO_ERROR;
		}
		bytes += written;
		remaining -= written;
	}
	if (fsync(fd) != 0) {
		int error = errno;
		close(fd);
		unlink(temporary.String());
		return (status_t)error;
	}
	close(fd);

	BPath target(path.String());
	BPath parentPath;
	status_t status = target.GetParent(&parentPath);
	if (status != B_OK)
		return status;
		// The Storage Kit replaces the old file atomically, and only after
		// the new one has been completely written and flushed.
	BEntry temporaryEntry(temporary.String());
	status = temporaryEntry.Rename(path.String(), true);
	if (status != B_OK) {
		BEntry leftover(temporary.String());
		leftover.Remove();
		return status;
	}
	return sync_directory(parentPath.Path());
}


status_t
PCloudCache::GetNode(const BString& id, PCloudNode& node)
{
	BPath path(fRoot.String());
	if (path.Append("meta") != B_OK || path.Append(id.String()) != B_OK)
		return B_ERROR;
	BString metaPath(path.Path());
	metaPath << ".meta";

	BFile file(metaPath.String(), B_READ_ONLY);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;
	off_t size = 0;
	status = file.GetSize(&size);
	if (status != B_OK || size <= 0 || size > 64 * 1024)
		return B_BAD_DATA;
	char* buffer = new(std::nothrow) char[size + 1];
	if (buffer == NULL)
		return B_NO_MEMORY;
	ssize_t bytes = file.Read(buffer, size);
	if (bytes < 0) {
		delete[] buffer;
		return (status_t)bytes;
	}
	buffer[bytes] = '\0';
	BMessage message;
	status = message.Unflatten(buffer);
	delete[] buffer;
	if (status != B_OK)
		return status;
	return node.FromMessage(message);
}


status_t
PCloudCache::PutNode(const PCloudNode& node)
{
	BMessage message;
	node.AddToMessage(message);
	char* buffer = NULL;
	ssize_t size = 0;
	status_t status = flatten_message(message, buffer, size);
	if (status != B_OK)
		return status;

	BPath path(fRoot.String());
	if (path.Append("meta") != B_OK || path.Append(node.fId.String()) != B_OK) {
		delete[] buffer;
		return B_ERROR;
	}
	BString metaPath(path.Path());
	metaPath << ".meta";
	status = _WriteAtomic(metaPath, buffer, size);
	delete[] buffer;
	return status;
}


status_t
PCloudCache::GetChildren(const BString& folderId,
	BObjectList<PCloudNode>& nodes, bigtime_t maxAge, bool& fresh)
{
	fresh = false;
	BPath path(fRoot.String());
	if (path.Append("children") != B_OK || path.Append(folderId.String())
		!= B_OK) {
		return B_ERROR;
	}
	BString listPath(path.Path());
	listPath << ".list";

	BFile file(listPath.String(), B_READ_ONLY);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;
	off_t size = 0;
	status = file.GetSize(&size);
	if (status != B_OK || size <= 0 || size > 4 * 1024 * 1024)
		return B_BAD_DATA;
	char* buffer = new(std::nothrow) char[size + 1];
	if (buffer == NULL)
		return B_NO_MEMORY;
	ssize_t bytes = file.Read(buffer, size);
	if (bytes < 0) {
		delete[] buffer;
		return (status_t)bytes;
	}
	buffer[bytes] = '\0';
	BMessage message;
	status = message.Unflatten(buffer);
	delete[] buffer;
	if (status != B_OK)
		return status;

	int64 saved = 0;
	message.FindInt64("saved", &saved);
	fresh = system_time() - saved < maxAge;

	int32 count = 0;
	message.FindInt32("count", &count);
	for (int32 i = 0; i < count; i++) {
		BMessage item;
		if (message.FindMessage("child", i, &item) != B_OK)
			continue;
		PCloudNode* node = new(std::nothrow) PCloudNode;
		if (node == NULL)
			return B_NO_MEMORY;
		if (node->FromMessage(item) == B_OK && nodes.AddItem(node))
			continue;
		delete node;
	}
	return B_OK;
}


status_t
PCloudCache::PutChildren(const BString& folderId,
	const BObjectList<PCloudNode>& nodes)
{
	BMessage message;
	message.AddInt64("saved", system_time());
	message.AddInt32("count", nodes.CountItems());
	for (int32 i = 0; i < nodes.CountItems(); i++) {
		BMessage item;
		nodes.ItemAt(i)->AddToMessage(item);
		message.AddMessage("child", &item);
	}
	char* buffer = NULL;
	ssize_t size = 0;
	status_t status = flatten_message(message, buffer, size);
	if (status != B_OK)
		return status;

	BPath path(fRoot.String());
	if (path.Append("children") != B_OK
		|| path.Append(folderId.String()) != B_OK) {
		delete[] buffer;
		return B_ERROR;
	}
	BString listPath(path.Path());
	listPath << ".list";
	status = _WriteAtomic(listPath, buffer, size);
	delete[] buffer;
	return status;
}


status_t
PCloudCache::TempPath(const BString& id, BString& path)
{
	BPath directory(fRoot.String());
	if (directory.Append("data") != B_OK)
		return B_ERROR;
	path.SetToFormat("%s/%s.tmp.%ld", directory.Path(), id.String(),
		(long)getpid());
	return B_OK;
}


status_t
PCloudCache::VerifyAndPromote(const BString& id, const BString& tempPath,
	const BString& sha1, off_t size)
{
	BFile file(tempPath.String(), B_READ_ONLY);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;

	off_t actualSize = 0;
	status = file.GetSize(&actualSize);
	if (status != B_OK || actualSize != size) {
		PCLOUD_LOG_ERROR("size mismatch for %s: got %" B_PRIdOFF
			", expected %" B_PRIdOFF, id.String(), actualSize, size);
		return B_BAD_DATA;
	}

	PCloudSHA1 hash;
	char buffer[kPCloudBufferSize];
	while (true) {
		ssize_t bytes = file.Read(buffer, sizeof(buffer));
		if (bytes < 0)
			return (status_t)bytes;
		if (bytes == 0)
			break;
		hash.Update(buffer, bytes);
	}
	uint8 digest[20];
	hash.Final(digest);
	BString actual = PCloudSHA1::ToHex(digest);
	if (sha1.Length() > 0 && actual != sha1) {
		PCLOUD_LOG_ERROR("checksum mismatch for %s: got %s, expected %s",
			id.String(), actual.String(), sha1.String());
		return B_BAD_DATA;
	}

	BPath directory(fRoot.String());
	if (directory.Append("data") != B_OK)
		return B_ERROR;
	BPath destination(directory);
	if (destination.Append(id.String()) != B_OK)
		return B_ERROR;

		// The temporary file passed size and checksum verification;
		// BEntry::Rename() replaces the old data atomically.
	BEntry temporaryEntry(tempPath.String());
	return temporaryEntry.Rename(destination.Path(), true);
	return sync_directory(directory.Path());
}


status_t
PCloudCache::CachedFile(const BString& id, const BString& sha1, off_t size,
	BString& path)
{
	BPath directory(fRoot.String());
	if (directory.Append("data") != B_OK)
		return B_ERROR;
	BPath file(directory);
	if (file.Append(id.String()) != B_OK)
		return B_ERROR;

	BEntry entry(file.Path());
	if (!entry.Exists())
		return B_ENTRY_NOT_FOUND;
	struct stat st;
	if (entry.GetStat(&st) != B_OK || st.st_size != size) {
		PCLOUD_LOG_WARNING("cached data for %s has wrong size", id.String());
		return B_BAD_DATA;
	}

	if (sha1.Length() > 0) {
		BFile data(file.Path(), B_READ_ONLY);
		status_t status = data.InitCheck();
		if (status != B_OK)
			return status;
		PCloudSHA1 hash;
		char buffer[kPCloudBufferSize];
		while (true) {
			ssize_t bytes = data.Read(buffer, sizeof(buffer));
			if (bytes < 0)
				return (status_t)bytes;
			if (bytes == 0)
				break;
			hash.Update(buffer, bytes);
		}
		uint8 digest[20];
		hash.Final(digest);
		if (PCloudSHA1::ToHex(digest) != sha1) {
			PCLOUD_LOG_WARNING("cached data for %s failed checksum",
				id.String());
			return B_BAD_DATA;
		}
	}

	path = file.Path();
	return B_OK;
}
