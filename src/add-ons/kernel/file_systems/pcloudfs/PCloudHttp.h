/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_HTTP_H
#define _PCLOUD_HTTP_H


#include "PCloudCrypto.h"

#include <String.h>


/*! Minimal HTTPS/1.1 client built on the native Be API classes
	BSecureSocket and BNetworkAddress.

	Only what the pCloud API needs is implemented: GET, POST with a form body,
	redirects, and streaming a response to a file while hashing it. Responses
	request identity encoding, so no decompressor is required.
*/
class PCloudHttp {
public:
						PCloudHttp();
						~PCloudHttp();

		status_t		Get(const BString& url, const BString& bearer,
							BString& body);
		status_t		PostForm(const BString& url, const BString& form,
							const BString& bearer, BString& body);

	/*! Streams the response body to an already open descriptor at fileOffset.
		Every received byte is passed to hasher. The descriptor is not closed
		and is not flushed by this method.
	*/
		status_t		DownloadToFile(const BString& url,
							const BString& bearer, int fd, off_t fileOffset,
							PCloudSHA1& hasher);

		int			LastStatusCode() const	{ return fStatusCode; }
		const BString&	LastError() const	{ return fLastError; }

private:
		status_t		_Request(const char* method, const BString& url,
							const BString& bearer, const BString& contentType,
							const BString& requestBody, BString& body,
							int fd, off_t fileOffset, PCloudSHA1* hasher);

		status_t		_Deliver(const void* data, size_t length,
							BString& body, int fd, off_t fileOffset,
							PCloudSHA1* hasher);

		void			_SetError(const char* format, ...);

private:
			int			fStatusCode;
			BString		fLastError;
};


#endif	// _PCLOUD_HTTP_H
