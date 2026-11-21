/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#define FUSE_USE_VERSION 26
#include <fuse_lowlevel.h>
#include <fuse_opt.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "PCloudFileSystem.h"
#include "PCloudLogger.h"


static PCloudFileSystem* sFileSystem = NULL;


static void
fs_init(void* userData, struct fuse_conn_info* connection)
{
	if (connection != NULL) {
		connection->max_write = 128 * 1024;
		connection->max_readahead = 128 * 1024;
	}
}


static void
fs_destroy(void* userData)
{
	PCLOUD_LOG_INFO("FUSE session destroyed");
}


static void
fs_lookup(fuse_req_t request, fuse_ino_t parent, const char* name)
{
	sFileSystem->Lookup(request, parent, name);
}


static void
fs_forget(fuse_req_t request, fuse_ino_t ino, unsigned long count)
{
	sFileSystem->Forget(request, ino, count);
}


static void
fs_getattr(fuse_req_t request, fuse_ino_t ino, struct fuse_file_info* info)
{
	sFileSystem->GetAttr(request, ino);
}


static void
fs_opendir(fuse_req_t request, fuse_ino_t ino, struct fuse_file_info* info)
{
	sFileSystem->OpenDir(request, ino);
}


static void
fs_readdir(fuse_req_t request, fuse_ino_t ino, size_t size, off_t offset,
	struct fuse_file_info* info)
{
	sFileSystem->ReadDir(request, ino, size, offset);
}


static void
fs_releasedir(fuse_req_t request, fuse_ino_t ino, struct fuse_file_info* info)
{
	sFileSystem->ReleaseDir(request, ino);
}


static void
fs_open(fuse_req_t request, fuse_ino_t ino, struct fuse_file_info* info)
{
	sFileSystem->Open(request, ino, info);
}


static void
fs_read(fuse_req_t request, fuse_ino_t ino, size_t size, off_t offset,
	struct fuse_file_info* info)
{
	sFileSystem->Read(request, ino, size, offset);
}


static void
fs_release(fuse_req_t request, fuse_ino_t ino, struct fuse_file_info* info)
{
	sFileSystem->Release(request, ino, info);
}


static void
fs_statfs(fuse_req_t request, fuse_ino_t ino)
{
	sFileSystem->StatFs(request, ino);
}


static void
fs_deny(fuse_req_t request)
{
	sFileSystem->DenyWrite(request);
}


static void
fs_write(fuse_req_t request, fuse_ino_t ino, const char* data, size_t size,
	off_t offset, struct fuse_file_info* info)
{
	fs_deny(request);
}


static void
fs_setattr(fuse_req_t request, fuse_ino_t ino, struct stat* attr, int toSet,
	struct fuse_file_info* info)
{
	fs_deny(request);
}


static void
fs_mkdir(fuse_req_t request, fuse_ino_t parent, const char* name, mode_t mode)
{
	fs_deny(request);
}


static void
fs_unlink(fuse_req_t request, fuse_ino_t parent, const char* name)
{
	fs_deny(request);
}


static void
fs_rmdir(fuse_req_t request, fuse_ino_t parent, const char* name)
{
	fs_deny(request);
}


static void
fs_rename(fuse_req_t request, fuse_ino_t parent, const char* name,
	fuse_ino_t newParent, const char* newName)
{
	fs_deny(request);
}


static void
fs_mknod(fuse_req_t request, fuse_ino_t parent, const char* name, mode_t mode,
	dev_t device)
{
	fs_deny(request);
}


static void
fs_symlink(fuse_req_t request, const char* link, fuse_ino_t parent,
	const char* name)
{
	fs_deny(request);
}


static void
fs_link(fuse_req_t request, fuse_ino_t ino, fuse_ino_t newParent,
	const char* newName)
{
	fs_deny(request);
}


static void
fs_create(fuse_req_t request, fuse_ino_t parent, const char* name, mode_t mode,
	struct fuse_file_info* info)
{
	fs_deny(request);
}


static void
fs_flush(fuse_req_t request, fuse_ino_t ino, struct fuse_file_info* info)
{
	fuse_reply_err(request, 0);
}


static void
fs_fsync(fuse_req_t request, fuse_ino_t ino, int datasync,
	struct fuse_file_info* info)
{
	fuse_reply_err(request, 0);
}


/*! Entry point called by userlandfs_server after it loads this add-on.

	The FUSE low-level interface is used because userlandfs_server implements
	it natively; the add-on itself is plain C++ using the Be API.
*/
extern "C" int
main(int argc, const char* const* argv)
{
	gPCloudLogger.Init("");

	PCloudFileSystem fileSystem;
	status_t status = fileSystem.Init();
	if (status != B_OK) {
		fprintf(stderr, "pcloudfs: initialization failed (status %ld)\n",
			(long)status);
		PCLOUD_LOG_ERROR("initialization failed: %ld", (long)status);
		return 1;
	}
	sFileSystem = &fileSystem;

	struct fuse_lowlevel_ops operations;
	memset(&operations, 0, sizeof(operations));
	operations.init = &fs_init;
	operations.destroy = &fs_destroy;
	operations.lookup = &fs_lookup;
	operations.forget = &fs_forget;
	operations.getattr = &fs_getattr;
	operations.opendir = &fs_opendir;
	operations.readdir = &fs_readdir;
	operations.releasedir = &fs_releasedir;
	operations.open = &fs_open;
	operations.read = &fs_read;
	operations.release = &fs_release;
	operations.statfs = &fs_statfs;

	// All mutating operations are explicitly refused in phase 1.
	operations.write = &fs_write;
	operations.setattr = &fs_setattr;
	operations.mkdir = &fs_mkdir;
	operations.unlink = &fs_unlink;
	operations.rmdir = &fs_rmdir;
	operations.rename = &fs_rename;
	operations.mknod = &fs_mknod;
	operations.symlink = &fs_symlink;
	operations.link = &fs_link;
	operations.create = &fs_create;
	operations.flush = &fs_flush;
	operations.fsync = &fs_fsync;

	struct fuse_args args = FUSE_ARGS_INIT(argc,
		const_cast<char**>(argv));
	struct fuse_session* session = fuse_lowlevel_new(&args, &operations,
		sizeof(operations), &fileSystem);
	if (session == NULL) {
		PCLOUD_LOG_ERROR("fuse_lowlevel_new() failed");
		fuse_opt_free_args(&args);
		sFileSystem = NULL;
		return 1;
	}

	PCLOUD_LOG_INFO("entering FUSE main loop");
	fuse_session_loop_mt(session);
	fuse_session_destroy(session);
	fuse_opt_free_args(&args);

	fileSystem.Destroy();
	sFileSystem = NULL;
	return 0;
}
