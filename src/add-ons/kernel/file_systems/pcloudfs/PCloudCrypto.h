/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_CRYPTO_H
#define _PCLOUD_CRYPTO_H


#include <SupportDefs.h>
#include <String.h>


/*! Streaming SHA-1.

	pCloud reports a SHA-1 checksum for every file. Every downloaded byte is
	fed through this class before the file is accepted into the cache.
*/
class PCloudSHA1 {
public:
						PCloudSHA1();

		void			Update(const void* data, size_t length);
		void			Final(uint8 digest[20]);

		static void		Hash(const void* data, size_t length, uint8 digest[20]);
		static BString	ToHex(const uint8 digest[20]);

private:
		void			_ProcessBlock(const uint8* block);

private:
			uint32		fState[5];
			uint64		fByteCount;
			uint8		fBuffer[64];
			size_t		fBufferUsed;
};


/*! Streaming SHA-256, used for the PKCE code challenge in OAuth2. */
class PCloudSHA256 {
public:
						PCloudSHA256();

		void			Update(const void* data, size_t length);
		void			Final(uint8 digest[32]);

		static void		Hash(const void* data, size_t length, uint8 digest[32]);

private:
		void			_ProcessBlock(const uint8* block);

private:
			uint32		fState[8];
			uint64		fByteCount;
			uint8		fBuffer[64];
			size_t		fBufferUsed;
};


/*! Base64 without padding, using the URL-safe alphabet (- and _). */
class PCloudCrypto {
public:
		static uint32	Crc32C(const void* data, size_t length);
};


class PCloudBase64Url {
public:
		static BString	Encode(const void* data, size_t length);
};


#endif	// _PCLOUD_CRYPTO_H
