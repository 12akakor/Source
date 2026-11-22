/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudCrypto.h"

#include <string.h>


static inline uint32
_LoadBigEndian32(const uint8* bytes)
{
	return ((uint32)bytes[0] << 24) | ((uint32)bytes[1] << 16)
		| ((uint32)bytes[2] << 8) | (uint32)bytes[3];
}


static inline void
_StoreBigEndian32(uint8* bytes, uint32 value)
{
	bytes[0] = (uint8)(value >> 24);
	bytes[1] = (uint8)(value >> 16);
	bytes[2] = (uint8)(value >> 8);
	bytes[3] = (uint8)value;
}


static inline uint32
_RotateLeft(uint32 value, uint32 bits)
{
	return (value << bits) | (value >> (32 - bits));
}


// #pragma mark - SHA-1


PCloudSHA1::PCloudSHA1()
	:
	fByteCount(0),
	fBufferUsed(0)
{
	fState[0] = 0x67452301;
	fState[1] = 0xefcdab89;
	fState[2] = 0x98badcfe;
	fState[3] = 0x10325476;
	fState[4] = 0xc3d2e1f0;
	memset(fBuffer, 0, sizeof(fBuffer));
}


void
PCloudSHA1::Update(const void* data, size_t length)
{
	const uint8* bytes = static_cast<const uint8*>(data);
	fByteCount += length;

	if (fBufferUsed > 0) {
		size_t needed = 64 - fBufferUsed;
		if (length < needed) {
			memcpy(fBuffer + fBufferUsed, bytes, length);
			fBufferUsed += length;
			return;
		}
		memcpy(fBuffer + fBufferUsed, bytes, needed);
		_ProcessBlock(fBuffer);
		fBufferUsed = 0;
		bytes += needed;
		length -= needed;
	}

	while (length >= 64) {
		_ProcessBlock(bytes);
		bytes += 64;
		length -= 64;
	}

	if (length > 0) {
		memcpy(fBuffer, bytes, length);
		fBufferUsed = length;
	}
}


void
PCloudSHA1::Final(uint8 digest[20])
{
	uint64 bitCount = fByteCount * 8;

	uint8 padding[64];
	memset(padding, 0, sizeof(padding));
	padding[0] = 0x80;
	size_t paddingLength = (fBufferUsed < 56 ? 56 : 120) - fBufferUsed;
	Update(padding, paddingLength);

	uint8 lengthBytes[8];
	for (int32 i = 0; i < 8; i++)
		lengthBytes[7 - i] = (uint8)(bitCount >> (i * 8));
	Update(lengthBytes, 8);

	for (int32 i = 0; i < 5; i++)
		_StoreBigEndian32(digest + i * 4, fState[i]);
}


void
PCloudSHA1::Hash(const void* data, size_t length, uint8 digest[20])
{
	PCloudSHA1 hash;
	hash.Update(data, length);
	hash.Final(digest);
}


BString
PCloudSHA1::ToHex(const uint8 digest[20])
{
	BString result;
	static const char kDigits[] = "0123456789abcdef";
	for (int32 i = 0; i < 20; i++) {
		result << kDigits[digest[i] >> 4];
		result << kDigits[digest[i] & 0x0f];
	}
	return result;
}


void
PCloudSHA1::_ProcessBlock(const uint8* block)
{
	uint32 words[80];
	for (int32 i = 0; i < 16; i++)
		words[i] = _LoadBigEndian32(block + i * 4);
	for (int32 i = 16; i < 80; i++) {
		words[i] = _RotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14]
			^ words[i - 16], 1);
	}

	uint32 a = fState[0];
	uint32 b = fState[1];
	uint32 c = fState[2];
	uint32 d = fState[3];
	uint32 e = fState[4];

	for (int32 i = 0; i < 80; i++) {
		uint32 function;
		uint32 constant;
		if (i < 20) {
			function = (b & c) | (~b & d);
			constant = 0x5a827999;
		} else if (i < 40) {
			function = b ^ c ^ d;
			constant = 0x6ed9eba1;
		} else if (i < 60) {
			function = (b & c) | (b & d) | (c & d);
			constant = 0x8f1bbcdc;
		} else {
			function = b ^ c ^ d;
			constant = 0xca62c1d6;
		}

		uint32 temporary = _RotateLeft(a, 5) + function + e + constant
			+ words[i];
		e = d;
		d = c;
		c = _RotateLeft(b, 30);
		b = a;
		a = temporary;
	}

	fState[0] += a;
	fState[1] += b;
	fState[2] += c;
	fState[3] += d;
	fState[4] += e;
}


// #pragma mark - SHA-256


static const uint32 kSHA256Constants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};


static inline uint32
_RotateRight(uint32 value, uint32 bits)
{
	return (value >> bits) | (value << (32 - bits));
}


PCloudSHA256::PCloudSHA256()
	:
	fByteCount(0),
	fBufferUsed(0)
{
	fState[0] = 0x6a09e667;
	fState[1] = 0xbb67ae85;
	fState[2] = 0x3c6ef372;
	fState[3] = 0xa54ff53a;
	fState[4] = 0x510e527f;
	fState[5] = 0x9b05688c;
	fState[6] = 0x1f83d9ab;
	fState[7] = 0x5be0cd19;
	memset(fBuffer, 0, sizeof(fBuffer));
}


void
PCloudSHA256::Update(const void* data, size_t length)
{
	const uint8* bytes = static_cast<const uint8*>(data);
	fByteCount += length;

	if (fBufferUsed > 0) {
		size_t needed = 64 - fBufferUsed;
		if (length < needed) {
			memcpy(fBuffer + fBufferUsed, bytes, length);
			fBufferUsed += length;
			return;
		}
		memcpy(fBuffer + fBufferUsed, bytes, needed);
		_ProcessBlock(fBuffer);
		fBufferUsed = 0;
		bytes += needed;
		length -= needed;
	}

	while (length >= 64) {
		_ProcessBlock(bytes);
		bytes += 64;
		length -= 64;
	}

	if (length > 0) {
		memcpy(fBuffer, bytes, length);
		fBufferUsed = length;
	}
}


void
PCloudSHA256::Final(uint8 digest[32])
{
	uint64 bitCount = fByteCount * 8;

	uint8 padding[64];
	memset(padding, 0, sizeof(padding));
	padding[0] = 0x80;
	size_t paddingLength = (fBufferUsed < 56 ? 56 : 120) - fBufferUsed;
	Update(padding, paddingLength);

	uint8 lengthBytes[8];
	for (int32 i = 0; i < 8; i++)
		lengthBytes[7 - i] = (uint8)(bitCount >> (i * 8));
	Update(lengthBytes, 8);

	for (int32 i = 0; i < 8; i++)
		_StoreBigEndian32(digest + i * 4, fState[i]);
}


void
PCloudSHA256::Hash(const void* data, size_t length, uint8 digest[32])
{
	PCloudSHA256 hash;
	hash.Update(data, length);
	hash.Final(digest);
}


void
PCloudSHA256::_ProcessBlock(const uint8* block)
{
	uint32 words[64];
	for (int32 i = 0; i < 16; i++)
		words[i] = _LoadBigEndian32(block + i * 4);
	for (int32 i = 16; i < 64; i++) {
		uint32 s0 = _RotateRight(words[i - 15], 7)
			^ _RotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
		uint32 s1 = _RotateRight(words[i - 2], 17)
			^ _RotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
		words[i] = words[i - 16] + s0 + words[i - 7] + s1;
	}

	uint32 a = fState[0];
	uint32 b = fState[1];
	uint32 c = fState[2];
	uint32 d = fState[3];
	uint32 e = fState[4];
	uint32 f = fState[5];
	uint32 g = fState[6];
	uint32 h = fState[7];

	for (int32 i = 0; i < 64; i++) {
		uint32 s1 = _RotateRight(e, 6) ^ _RotateRight(e, 11)
			^ _RotateRight(e, 25);
		uint32 choice = (e & f) ^ (~e & g);
		uint32 temporary1 = h + s1 + choice + kSHA256Constants[i] + words[i];
		uint32 s0 = _RotateRight(a, 2) ^ _RotateRight(a, 13)
			^ _RotateRight(a, 22);
		uint32 majority = (a & b) ^ (a & c) ^ (b & c);
		uint32 temporary2 = s0 + majority;

		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}

	fState[0] += a;
	fState[1] += b;
	fState[2] += c;
	fState[3] += d;
	fState[4] += e;
	fState[5] += f;
	fState[6] += g;
	fState[7] += h;
}


// #pragma mark - Base64 URL


uint32
PCloudCrypto::Crc32C(const void* data, size_t length)
{
	static const uint32 kPolynomial = 0x82f63b78;
	const uint8* bytes = static_cast<const uint8*>(data);
	uint32 crc = 0xffffffff;
	for (size_t i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (int32 bit = 0; bit < 8; bit++) {
			if ((crc & 1) != 0)
				crc = (crc >> 1) ^ kPolynomial;
			else
				crc >>= 1;
		}
	}
	return crc ^ 0xffffffff;
}


BString
PCloudBase64Url::Encode(const void* data, size_t length)
{
	static const char kAlphabet[]
		= "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	const uint8* bytes = static_cast<const uint8*>(data);

	BString result;
	for (size_t i = 0; i < length; i += 3) {
		uint32 value = (uint32)bytes[i] << 16;
		if (i + 1 < length)
			value |= (uint32)bytes[i + 1] << 8;
		if (i + 2 < length)
			value |= (uint32)bytes[i + 2];

		result << kAlphabet[(value >> 18) & 0x3f];
		result << kAlphabet[(value >> 12) & 0x3f];
		if (i + 1 < length)
			result << kAlphabet[(value >> 6) & 0x3f];
		if (i + 2 < length)
			result << kAlphabet[value & 0x3f];
	}
	return result;
}
