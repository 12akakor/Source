/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_API_H
#define _PCLOUD_API_H


#include "PCloudAuth.h"
#include "PCloudCrypto.h"
#include "PCloudHttp.h"
#include "PCloudNode.h"

#include <ObjectList.h>
#include <String.h>


/*! The small subset of the pCloud HTTP JSON API used by the filesystem.

	Every call checks the pCloud `result` field as well as the HTTP status,
	and reports a human readable reason in LastError(). Downloads are always
	paired with checksumfile() so the cache can verify what arrived.
*/
class PCloudApi {
public:
						PCloudApi(PCloudAuth& auth);

		status_t		UserInfo(BString& email, off_t& quota, off_t& used);
		status_t		ListFolder(const BString& folderId,
							BObjectList<PCloudNode>& nodes);
		status_t		Checksum(const BString& fileId, BString& sha1,
							off_t& size);

	/*! Downloads a file directly to `destinationPath`. The SHA-1 of the
		received bytes is returned in `hash`; the caller verifies it against
		Checksum() before renaming the temporary file.
	*/
		status_t		Download(const BString& fileId,
							const BString& destinationPath,
							PCloudSHA1& hash);

		const BString&	LastError() const	{ return fLastError; }

private:
		status_t		_Call(const char* method, const char* parameters,
							BMessage& reply);
		void			_SetError(const char* format, ...);

private:
			PCloudAuth&	fAuth;
			PCloudHttp	fHttp;
			BString		fLastError;
};


#endif	// _PCLOUD_API_H
