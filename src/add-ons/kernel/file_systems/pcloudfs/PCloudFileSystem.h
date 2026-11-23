/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_FILE_SYSTEM_H
#define _PCLOUD_FILE_SYSTEM_H


#define FUSE_USE_VERSION 26
#include <fuse_lowlevel.h>

#include "PCloudApi.h"
#include "PCloudAuth.h"
#include "PCloudCache.h"
#include "PCloudNode.h"

#include <Locker.h>
#include <ObjectList.h>
#include <String.h>

#include <map>


/*! The UserlandFS add-on implementation.

	One instance lives for the lifetime of the mount. All FUSE callbacks are
	threaded by userlandfs_server, so state shared between callbacks is
	protected by fLock. Node ids from pCloud are mapped to FUSE inode numbers;
	the mapping is stable for the duration of the mount.

	Phase 1 implements read operations only. Every operation that could modify
	remote data returns EROFS until the write path (with its write-ahead
	journal) has been implemented and tested.
*/
class PCloudFileSystem {
public:
	struct Node {
		fuse_ino_t			ino;
		BString				id;
		BString				name;
		BString				parentId;
		bool					isFolder;
		off_t				size;
		int64				modified;
		BString				sha1;
		bigtime_t			refreshed;
		int					fd;
		unsigned long		lookupCount;

		Node();
		~Node();
	};

public:
						PCloudFileSystem();
						~PCloudFileSystem();

		status_t		Init();
		void			Destroy();

		void			Lookup(fuse_req_t request, fuse_ino_t parent,
							const char* name);
		void			Forget(fuse_req_t request, fuse_ino_t ino,
							unsigned long count);
		void			GetAttr(fuse_req_t request, fuse_ino_t ino);
		void			OpenDir(fuse_req_t request, fuse_ino_t ino);
		void			ReadDir(fuse_req_t request, fuse_ino_t ino,
							size_t size, off_t offset);
		void			ReleaseDir(fuse_req_t request, fuse_ino_t ino);
		void			Open(fuse_req_t request, fuse_ino_t ino,
							struct fuse_file_info* info);
		void			Read(fuse_req_t request, fuse_ino_t ino, size_t size,
							off_t offset);
		void			Release(fuse_req_t request, fuse_ino_t ino,
							struct fuse_file_info* info);
		void			StatFs(fuse_req_t request, fuse_ino_t ino);

		void			DenyWrite(fuse_req_t request);

private:
		Node*			_NodeForIno(fuse_ino_t ino);
		Node*			_NodeForId(const BString& id);
		Node*			_AddNode(const PCloudNode& node);
		fuse_ino_t		_NextIno();

		status_t		_Children(fuse_ino_t folderIno,
							BObjectList<PCloudNode>& children);
		status_t		_EnsureFile(Node* node, BString& path);
		void			_FillStat(const Node* node, struct stat& st);
		void			_ReplyError(fuse_req_t request, status_t status);

private:
			BLocker		fLock;
			PCloudAuth	fAuth;
			PCloudApi	fApi;
			PCloudCache	fCache;
			BObjectList<Node> fNodes;
			std::map<BString, Node*> fNodesById;
			fuse_ino_t	fNextIno;
			bool		fInitialized;
};


#endif	// _PCLOUD_FILE_SYSTEM_H
