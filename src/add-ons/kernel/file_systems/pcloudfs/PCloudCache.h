/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_CACHE_H
#define _PCLOUD_CACHE_H


#include "PCloudNode.h"

#include <Message.h>
#include <ObjectList.h>
#include <String.h>


/*! On-disk cache with integrity checks.

	Every update is written to a temporary file, flushed with fsync(), and
	only then renamed over the old value. A crash can therefore never expose
	a partially written file or metadata record. Downloaded data is verified
	against the SHA-1 from pCloud before it is promoted into the data
	directory.
*/
class PCloudCache {
public:
						PCloudCache();

		status_t		Init(const BString& accountTag);
		const BString&	Root() const	{ return fRoot; }

		status_t		GetNode(const BString& id, PCloudNode& node);
		status_t		PutNode(const PCloudNode& node);

		status_t		GetChildren(const BString& folderId,
							BObjectList<PCloudNode>& nodes,
							bigtime_t maxAge, bool& fresh);
		status_t		PutChildren(const BString& folderId,
							const BObjectList<PCloudNode>& nodes);

		status_t		TempPath(const BString& id, BString& path);
		status_t		VerifyAndPromote(const BString& id,
							const BString& tempPath, const BString& sha1,
							off_t size);
		status_t		CachedFile(const BString& id, const BString& sha1,
							off_t size, BString& path);

private:
		status_t		_EnsureDirectories();
		status_t		_WriteAtomic(const BString& path, const void* data,
							size_t size);

private:
			BString		fRoot;
};


#endif	// _PCLOUD_CACHE_H
