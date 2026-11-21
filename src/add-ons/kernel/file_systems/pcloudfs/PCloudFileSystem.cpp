/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudFileSystem.h"

#include "PCloudDefs.h"
#include "PCloudLogger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <new>

#include <AutoLocker.h>
#include <Entry.h>
#include <Path.h>


PCloudFileSystem::Node::Node()
	:
	ino(0),
	id(),
	name(),
	parentId(),
	isFolder(false),
	size(0),
	modified(0),
	sha1(),
	refreshed(0),
	fd(-1),
	lookupCount(0)
{
}


PCloudFileSystem::Node::~Node()
{
	if (fd >= 0)
		close(fd);
}


PCloudFileSystem::PCloudFileSystem()
	:
	fLock(),
	fAuth(),
	fApi(fAuth),
	fCache(),
	fNodes(),
	fNodesById(),
	fNextIno(FUSE_ROOT_ID + 1),
	fInitialized(false)
{
}


PCloudFileSystem::~PCloudFileSystem()
{
	Destroy();
}


status_t
PCloudFileSystem::Init()
{
	PCLOUD_LOG_INFO("initializing pcloudfs");

	status_t status = fAuth.Init();
	if (status != B_OK) {
		PCLOUD_LOG_ERROR("could not read settings: %ld", (long)status);
		return status;
	}
	if (!fAuth.IsConfigured()) {
		PCLOUD_LOG_ERROR("pcloudfs is not configured; run pcloudctl setup");
		return B_PERMISSION_DENIED;
	}
	if (!fAuth.HasToken()) {
		PCLOUD_LOG_ERROR("no pCloud access token; run pcloudctl setup");
		return B_PERMISSION_DENIED;
	}

	status = fCache.Init(fAuth.Uid().Length() > 0 ? fAuth.Uid() : BString("0"));
	if (status != B_OK) {
		PCLOUD_LOG_ERROR("could not initialize cache: %ld", (long)status);
		return status;
	}

	PCloudNode root;
	root.fId = "0";
	root.fName = "/";
	root.fParentId = "0";
	root.fIsFolder = true;
	root.fRefreshed = system_time();
	Node* rootNode = _AddNode(root);
	if (rootNode == NULL)
		return B_NO_MEMORY;
	rootNode->ino = FUSE_ROOT_ID;
	fNodesById[BString("0")] = rootNode;

	fInitialized = true;
	PCLOUD_LOG_INFO("pcloudfs ready (cache: %s)", fCache.Root().String());
	return B_OK;
}


void
PCloudFileSystem::Destroy()
{
	if (!fInitialized)
		return;
	AutoLocker<BLocker> locker(&fLock);
	for (int32 i = 0; i < fNodes.CountItems(); i++) {
		Node* node = fNodes.ItemAt(i);
		if (node != NULL && node->fd >= 0) {
			close(node->fd);
			node->fd = -1;
		}
	}
	fInitialized = false;
	PCLOUD_LOG_INFO("pcloudfs stopped");
}


PCloudFileSystem::Node*
PCloudFileSystem::_NodeForIno(fuse_ino_t ino)
{
	for (int32 i = 0; i < fNodes.CountItems(); i++) {
		Node* node = fNodes.ItemAt(i);
		if (node != NULL && node->ino == ino)
			return node;
	}
	return NULL;
}


PCloudFileSystem::Node*
PCloudFileSystem::_NodeForId(const BString& id)
{
	std::map<BString, Node*>::iterator found = fNodesById.find(id);
	return found != fNodesById.end() ? found->second : NULL;
}


fuse_ino_t
PCloudFileSystem::_NextIno()
{
	if (fNextIno == FUSE_ROOT_ID)
		fNextIno++;
	return fNextIno++;
}


PCloudFileSystem::Node*
PCloudFileSystem::_AddNode(const PCloudNode& node)
{
	Node* result = _NodeForId(node.fId);
	if (result == NULL) {
		result = new(std::nothrow) Node;
		if (result == NULL)
			return NULL;
		result->ino = _NextIno();
		result->id = node.fId;
		if (!fNodes.AddItem(result)) {
			delete result;
			return NULL;
		}
		fNodesById[node.fId] = result;
	}

	result->name = node.fName;
	result->parentId = node.fParentId;
	result->isFolder = node.fIsFolder;
	result->size = node.fSize;
	result->modified = node.fModified;
	result->sha1 = node.fSha1;
	result->refreshed = node.fRefreshed;
	return result;
}


status_t
PCloudFileSystem::_Children(fuse_ino_t folderIno,
	BObjectList<PCloudNode>& children)
{
	Node* folder = _NodeForIno(folderIno);
	if (folder == NULL)
		return B_ENTRY_NOT_FOUND;
	if (!folder->isFolder)
		return B_BAD_VALUE;

	BObjectList<PCloudNode> cached;
	bool fresh = false;
	status_t status = fCache.GetChildren(folder->id, cached,
		kPCloudMetadataTimeout, fresh);
	if (status == B_OK && fresh) {
		for (int32 i = 0; i < cached.CountItems(); i++) {
			PCloudNode* item = cached.ItemAt(i);
			if (item == NULL || item->fId.Length() == 0)
				continue;
			PCloudNode* copy = new(std::nothrow) PCloudNode(*item);
			if (copy == NULL || !children.AddItem(copy)) {
				delete copy;
				return B_NO_MEMORY;
			}
		}
		return B_OK;
	}

	BObjectList<PCloudNode> remote;
	status = fApi.ListFolder(folder->id, remote);
	if (status != B_OK)
		return status;

	fCache.PutChildren(folder->id, remote);
	for (int32 i = 0; i < remote.CountItems(); i++) {
		if (remote.ItemAt(i) != NULL)
			children.AddItem(remote.ItemAt(i));
	}
	remote.MakeEmpty(false);
	return B_OK;
}


status_t
PCloudFileSystem::_EnsureFile(Node* node, BString& path)
{
	PCLOUD_LOG_DEBUG("ensure file %s", node->id.String());

	BString sha1;
	off_t size = 0;
	status_t status = fApi.Checksum(node->id, sha1, size);
	if (status != B_OK)
		return status;
	node->sha1 = sha1;
	node->size = size;

	status = fCache.CachedFile(node->id, sha1, size, path);
	if (status == B_OK)
		return B_OK;
	if (status != B_ENTRY_NOT_FOUND && status != B_BAD_DATA) {
		PCLOUD_LOG_WARNING("cache check for %s: %ld", node->id.String(),
			(long)status);
	}

	BString temporaryPath;
	status = fCache.TempPath(node->id, temporaryPath);
	if (status != B_OK)
		return status;
	temporaryPath << '.' << (int32)getpid();

	PCloudSHA1 receivedHash;
	status = fApi.Download(node->id, temporaryPath, receivedHash);
	if (status != B_OK) {
		PCLOUD_LOG_ERROR("download of %s failed: %s", node->id.String(),
			fApi.LastError().String());
		return status;
	}

	PCloudSHA1 verificationCopy(receivedHash);
	uint8 receivedDigest[20];
	verificationCopy.Final(receivedDigest);
	if (PCloudSHA1::ToHex(receivedDigest) != sha1) {
		PCLOUD_LOG_ERROR("download of %s failed checksum verification",
			node->id.String());
		BEntry entry(temporaryPath.String());
		entry.Remove();
		return B_BAD_DATA;
	}

	status = fCache.VerifyAndPromote(node->id, temporaryPath, sha1, size);
	if (status != B_OK) {
		BEntry entry(temporaryPath.String());
		entry.Remove();
		return status;
	}

	return fCache.CachedFile(node->id, sha1, size, path);
}


void
PCloudFileSystem::_FillStat(const Node* node, struct stat& st)
{
	memset(&st, 0, sizeof(st));
	st.st_ino = node->ino;
	st.st_mode = node->isFolder ? S_IFDIR | 0755 : S_IFREG | 0444;
	st.st_nlink = node->isFolder ? 2 : 1;
	st.st_uid = getuid();
	st.st_gid = getgid();
	st.st_size = node->size;
	st.st_blksize = 4096;
	st.st_blocks = (node->size + 511) / 512;
	st.st_atime = node->modified;
	st.st_mtime = node->modified;
	st.st_ctime = node->modified;
}


void
PCloudFileSystem::_ReplyError(fuse_req_t request, status_t status)
{
	int error;
	switch (status) {
		case B_ENTRY_NOT_FOUND:		error = ENOENT; break;
		case B_PERMISSION_DENIED:	error = EACCES; break;
		case B_BAD_VALUE:			error = EINVAL; break;
		case B_NO_MEMORY:			error = ENOMEM; break;
		case B_TIMED_OUT:			error = ETIMEDOUT; break;
		case B_BUSY:				error = EBUSY; break;
		case B_NOT_ALLOWED:		error = EPERM; break;
		default:				error = EIO; break;
	}
	fuse_reply_err(request, error);
}


void
PCloudFileSystem::Lookup(fuse_req_t request, fuse_ino_t parent,
	const char* name)
{
	AutoLocker<BLocker> locker(&fLock);
	BObjectList<PCloudNode> children;
	status_t status = _Children(parent, children);
	if (status != B_OK) {
		_ReplyError(request, status);
		return;
	}

	for (int32 i = 0; i < children.CountItems(); i++) {
		PCloudNode* child = children.ItemAt(i);
		if (child == NULL || child->fName != name)
			continue;
		Node* node = _AddNode(*child);
		if (node == NULL) {
			_ReplyError(request, B_NO_MEMORY);
			return;
		}
		node->lookupCount++;

		struct fuse_entry_param entry;
		memset(&entry, 0, sizeof(entry));
		entry.ino = node->ino;
		entry.generation = 1;
		_FillStat(node, entry.attr);
		entry.attr_timeout = kPCloudMetadataTimeout / 1000000.0;
		entry.entry_timeout = kPCloudMetadataTimeout / 1000000.0;
		fuse_reply_entry(request, &entry);
		return;
	}

	_ReplyError(request, B_ENTRY_NOT_FOUND);
}


void
PCloudFileSystem::Forget(fuse_req_t request, fuse_ino_t ino,
	unsigned long count)
{
	AutoLocker<BLocker> locker(&fLock);
	Node* node = _NodeForIno(ino);
	if (node != NULL && node->lookupCount >= count)
		node->lookupCount -= count;
	// UserlandFS does not implement fuse_reply_none(); a zero-error reply
	// is sufficient and lets the request finish.
	fuse_reply_err(request, 0);
}


void
PCloudFileSystem::GetAttr(fuse_req_t request, fuse_ino_t ino)
{
	AutoLocker<BLocker> locker(&fLock);
	Node* node = _NodeForIno(ino);
	if (node == NULL) {
		_ReplyError(request, B_ENTRY_NOT_FOUND);
		return;
	}
	struct stat st;
	_FillStat(node, st);
	fuse_reply_attr(request, &st, kPCloudMetadataTimeout / 1000000.0);
}


void
PCloudFileSystem::OpenDir(fuse_req_t request, fuse_ino_t ino)
{
	AutoLocker<BLocker> locker(&fLock);
	Node* folder = _NodeForIno(ino);
	if (folder == NULL || !folder->isFolder) {
		_ReplyError(request, B_ENTRY_NOT_FOUND);
		return;
	}
	struct fuse_file_info info;
	memset(&info, 0, sizeof(info));
	fuse_reply_open(request, &info);
}


void
PCloudFileSystem::ReadDir(fuse_req_t request, fuse_ino_t ino, size_t size,
	off_t offset)
{
	AutoLocker<BLocker> locker(&fLock);
	BObjectList<PCloudNode> children;
	status_t status = _Children(ino, children);
	if (status != B_OK) {
		_ReplyError(request, status);
		return;
	}

	char* buffer = new(std::nothrow) char[size];
	if (buffer == NULL) {
		_ReplyError(request, B_NO_MEMORY);
		return;
	}

	size_t used = 0;
	off_t position = 0;

	struct stat st;
	memset(&st, 0, sizeof(st));
	st.st_mode = S_IFDIR | 0755;
	st.st_ino = ino;

	if (position >= offset) {
		size_t entrySize = fuse_add_direntry(request, NULL, 0, ".", &st, 0);
		if (used + entrySize <= size)
			used += fuse_add_direntry(request, buffer + used, size - used,
				".", &st, position + entrySize);
	}
	position++;

	if (position >= offset) {
		size_t entrySize = fuse_add_direntry(request, NULL, 0, "..", &st, 0);
		if (used + entrySize <= size)
			used += fuse_add_direntry(request, buffer + used, size - used,
				"..", &st, position + entrySize);
	}
	position++;

	for (int32 i = 0; i < children.CountItems(); i++) {
		PCloudNode* child = children.ItemAt(i);
		if (child == NULL)
			continue;
		Node* node = _AddNode(*child);
		if (node == NULL)
			continue;

		if (position >= offset) {
			struct stat childStat;
			_FillStat(node, childStat);
			size_t entrySize = fuse_add_direntry(request, NULL, 0,
				child->fName.String(), &childStat, 0);
			if (used + entrySize <= size) {
				used += fuse_add_direntry(request, buffer + used, size - used,
					child->fName.String(), &childStat, position + entrySize);
			}
		}
		position++;
	}

	fuse_reply_buf(request, buffer, used);
	delete[] buffer;
}


void
PCloudFileSystem::ReleaseDir(fuse_req_t request, fuse_ino_t ino)
{
	fuse_reply_err(request, 0);
}


void
PCloudFileSystem::Open(fuse_req_t request, fuse_ino_t ino,
	struct fuse_file_info* info)
{
	AutoLocker<BLocker> locker(&fLock);
	Node* node = _NodeForIno(ino);
	if (node == NULL) {
		_ReplyError(request, B_ENTRY_NOT_FOUND);
		return;
	}
	if (node->isFolder) {
		fuse_reply_err(request, EISDIR);
		return;
	}
	if ((info->flags & O_ACCMODE) != O_RDONLY) {
		fuse_reply_err(request, EROFS);
		return;
	}
	if (node->fd >= 0) {
		info->fh = node->fd;
		info->direct_io = 1;
		fuse_reply_open(request, info);
		return;
	}

	BString path;
	status_t status = _EnsureFile(node, path);
	if (status != B_OK) {
		_ReplyError(request, status);
		return;
	}

	int fd = open(path.String(), O_RDONLY);
	if (fd < 0) {
		_ReplyError(request, B_IO_ERROR);
		return;
	}
	node->fd = fd;
	info->fh = fd;
	info->direct_io = 1;
	fuse_reply_open(request, info);
}


void
PCloudFileSystem::Read(fuse_req_t request, fuse_ino_t ino, size_t size,
	off_t offset)
{
	AutoLocker<BLocker> locker(&fLock);
	Node* node = _NodeForIno(ino);
	if (node == NULL || node->fd < 0) {
		_ReplyError(request, B_BAD_VALUE);
		return;
	}
	if (size > 1024 * 1024)
		size = 1024 * 1024;

	char* buffer = new(std::nothrow) char[size];
	if (buffer == NULL) {
		_ReplyError(request, B_NO_MEMORY);
		return;
	}
	ssize_t bytes = pread(node->fd, buffer, size, offset);
	if (bytes < 0) {
		delete[] buffer;
		_ReplyError(request, B_IO_ERROR);
		return;
	}
	fuse_reply_buf(request, buffer, (size_t)bytes);
	delete[] buffer;
}


void
PCloudFileSystem::Release(fuse_req_t request, fuse_ino_t ino,
	struct fuse_file_info* info)
{
	AutoLocker<BLocker> locker(&fLock);
	Node* node = _NodeForIno(ino);
	if (node != NULL && node->fd == (int)info->fh) {
		close(node->fd);
		node->fd = -1;
	}
	info->fh = 0;
	fuse_reply_err(request, 0);
}


void
PCloudFileSystem::StatFs(fuse_req_t request, fuse_ino_t ino)
{
	BString email;
	off_t quota = 0;
	off_t used = 0;
	status_t status = fApi.UserInfo(email, quota, used);
	if (status != B_OK) {
		_ReplyError(request, status);
		return;
	}

	struct statvfs info;
	memset(&info, 0, sizeof(info));
	info.f_bsize = 4096;
	info.f_frsize = 4096;
	info.f_blocks = quota / 4096;
	info.f_bfree = (quota - used) / 4096;
	info.f_bavail = info.f_bfree;
	info.f_namemax = B_FILE_NAME_LENGTH;
	fuse_reply_statfs(request, &info);
}


void
PCloudFileSystem::DenyWrite(fuse_req_t request)
{
	fuse_reply_err(request, EROFS);
}
