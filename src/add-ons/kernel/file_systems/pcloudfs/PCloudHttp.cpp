/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "PCloudHttp.h"

#include "PCloudDefs.h"
#include "PCloudLogger.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <NetworkAddress.h>
#include <SecureSocket.h>


struct HttpUrl {
	BString host;
	uint16 port;
	BString path;
};


static status_t
parse_url(const BString& url, HttpUrl& result)
{
	BString rest(url);
	if (rest.StartsWith("https://")) {
		rest.Remove(0, 8);
		result.port = kPCloudApiPort;
	} else if (rest.StartsWith("http://")) {
		// Plain HTTP is not used by this client; reject it explicitly.
		return B_BAD_VALUE;
	} else {
		result.port = kPCloudApiPort;
	}

	int32 pathStart = rest.FindFirst('/');
	if (pathStart < 0) {
		result.host = rest;
		result.path = "/";
	} else {
		rest.MoveInto(result.host, 0, pathStart);
		result.path.SetTo(rest.String(), rest.Length());
	}
	if (result.host.Length() == 0)
		return B_BAD_VALUE;

	int32 colon = result.host.FindLast(':');
	if (colon >= 0) {
		BString portString;
		result.host.MoveInto(portString, colon, result.host.Length() - colon);
		result.host.Truncate(colon);
		long port = strtol(portString.String() + 1, NULL, 10);
		if (port <= 0 || port > 65535)
			return B_BAD_VALUE;
		result.port = (uint16)port;
	}
		// A host without a scheme still needs a path.
	if (!result.path.StartsWith("/"))
		result.path.Prepend("/");
	return B_OK;
}


static bool
header_value(const BString& headers, const char* name, BString& value)
{
	BString search;
	search.SetToFormat("\n%s:", name);
	int32 found = headers.FindFirst(search.String());
	if (found < 0 && headers.StartsWith(name))
		found = -1;
	if (found < 0)
		return false;

	int32 start = found < 0 ? 0 : found + search.Length();
	int32 end = headers.FindFirst('\n', start);
	if (end < 0)
		end = headers.Length();
	value.SetTo(headers.String() + start, end - start);
	value.Trim();
	return true;
}


status_t
PCloudHttp::_Deliver(const void* data, size_t length, BString& body, int fd,
	off_t fileOffset, PCloudSHA1* hasher)
{
	if (hasher != NULL)
		hasher->Update(data, length);

	if (fd >= 0) {
		const char* bytes = static_cast<const char*>(data);
		size_t remaining = length;
		while (remaining > 0) {
			ssize_t written = pwrite(fd, bytes, remaining, fileOffset);
			if (written <= 0) {
				_SetError("writing cache file failed: %s",
					strerror(written < 0 ? (int)written : B_ERROR));
				return written < 0 ? (status_t)written : B_IO_ERROR;
			}
			bytes += written;
			fileOffset += written;
			remaining -= written;
		}
		return B_OK;
	}

	if (body.Length() + length > kPCloudMaxResponseSize) {
		_SetError("response too large");
		return B_BAD_DATA;
	}
	body.Append(static_cast<const char*>(data), (int32)length);
	return B_OK;
}


PCloudHttp::PCloudHttp()
	:
	fStatusCode(0)
{
}


PCloudHttp::~PCloudHttp()
{
}


status_t
PCloudHttp::Get(const BString& url, const BString& bearer, BString& body)
{
	return _Request("GET", url, bearer, "", "", body, -1, 0, NULL);
}


status_t
PCloudHttp::PostForm(const BString& url, const BString& form,
	const BString& bearer, BString& body)
{
	return _Request("POST", url, bearer, "application/x-www-form-urlencoded",
		form, body, -1, 0, NULL);
}


status_t
PCloudHttp::DownloadToFile(const BString& url, const BString& bearer, int fd,
	off_t fileOffset, PCloudSHA1& hasher)
{
	BString body;
	return _Request("GET", url, bearer, "", "", body, fd, fileOffset,
		&hasher);
}


void
PCloudHttp::_SetError(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	fLastError.SetToFormatVarArgs(format, args);
	va_end(args);
	PCLOUD_LOG_ERROR("HTTP: %s", fLastError.String());
}


status_t
PCloudHttp::_Request(const char* method, const BString& url,
	const BString& bearer, const BString& contentType,
	const BString& requestBody, BString& body, int fd, off_t fileOffset,
	PCloudSHA1* hasher)
{
	BString currentUrl(url);
	for (int32 redirect = 0; redirect < 5; redirect++) {
		HttpUrl target;
		status_t status = parse_url(currentUrl, target);
		if (status != B_OK) {
			_SetError("invalid URL: %s", currentUrl.String());
			return status;
		}

		BNetworkAddress address(AF_INET, target.host.String(), target.port);
		status = address.InitCheck();
		if (status != B_OK) {
			address.SetTo(target.host.String(), target.port);
			status = address.InitCheck();
		}
		if (status != B_OK) {
			_SetError("could not resolve %s: %s", target.host.String(),
				strerror(status));
			return status;
		}
		PCLOUD_LOG_INFO("HTTP: %s resolved to %s", target.host.String(),
			address.ToString().String());

		BSecureSocket socket(address, kPCloudConnectTimeout);
		status = socket.InitCheck();
		if (status != B_OK || !socket.IsConnected()) {
			_SetError("TLS connect failed for %s (%s): %s",
				target.host.String(), address.ToString().String(),
				strerror(status == B_OK ? B_TIMED_OUT : status));
			return status == B_OK ? B_TIMED_OUT : status;
		}
		PCLOUD_LOG_INFO("HTTP: TLS connected to %s", address.ToString().String());
		status = socket.SetTimeout(kPCloudRequestTimeout);
		if (status != B_OK) {
			_SetError("could not set socket timeout: %s", strerror(status));
			return status;
		}

		BString request;
		request.SetToFormat("%s %s HTTP/1.1\r\n"
			"Host: %s\r\n"
			"User-Agent: pcloudfs/0.1 (RenkuOS)\r\n"
			"Accept: */*\r\n"
			"Accept-Encoding: identity\r\n"
			"Connection: close\r\n", method, target.path.String(),
			target.host.String());
		if (bearer.Length() > 0) {
			request << "Authorization: Bearer " << bearer << "\r\n";
		}
		if (contentType.Length() > 0) {
			request << "Content-Type: " << contentType << "\r\n";
			request << "Content-Length: " << (int32)requestBody.Length()
				<< "\r\n";
		}
		request << "\r\n";

		const char* data = request.String();
		size_t remaining = request.Length();
		while (remaining > 0) {
			ssize_t written = socket.Write(data, remaining);
			if (written <= 0) {
				_SetError("sending request to %s failed: %s",
					target.host.String(), strerror(written < 0
						? (int)written : B_ERROR));
				return written < 0 ? (status_t)written : B_ERROR;
			}
			data += written;
			remaining -= written;
		}

		if (contentType.Length() > 0 && requestBody.Length() > 0) {
			data = requestBody.String();
			remaining = requestBody.Length();
			while (remaining > 0) {
				ssize_t written = socket.Write(data, remaining);
				if (written <= 0) {
					_SetError("sending request body failed: %s",
						strerror(written < 0 ? (int)written : B_ERROR));
					return written < 0 ? (status_t)written : B_ERROR;
				}
				data += written;
				remaining -= written;
			}
		}

		BString headers;
		char character;
		int32 headerEnd = -1;
		while (true) {
			ssize_t read = socket.Read(&character, 1);
			if (read <= 0) {
				_SetError("no response from %s: %s", target.host.String(),
					strerror(read < 0 ? (int)read : B_ERROR));
				return read < 0 ? (status_t)read : B_ERROR;
			}
			headers.Append(&character, 1);
			if (headers.Length() > 64 * 1024) {
				_SetError("response headers too large");
				return B_BAD_DATA;
			}
			if (headers.FindFirst("\r\n\r\n", headers.Length() - 3) >= 0) {
				headerEnd = headers.Length() - 2;
				break;
			}
		}
		headers.Truncate(headerEnd);

		int statusCode = 0;
		sscanf(headers.String(), "HTTP/1.%*d %d", &statusCode);
		fStatusCode = statusCode;
		BString location;
		bool isRedirect = statusCode == 301 || statusCode == 302
			|| statusCode == 303 || statusCode == 307
			|| statusCode == 308;
		if (isRedirect && header_value(headers, "Location", location)) {
			currentUrl = location;
			continue;
		}

		if (statusCode < 200 || statusCode >= 300) {
			_SetError("HTTP status %d from %s", statusCode,
				target.host.String());
			return B_ERROR;
		}

		BString transferEncoding;
		header_value(headers, "Transfer-Encoding", transferEncoding);
		bool chunked = transferEncoding.ICompare("chunked") == 0;

		BString contentLengthString;
		off_t contentLength = -1;
		if (header_value(headers, "Content-Length", contentLengthString))
			contentLength = strtoll(contentLengthString.String(), NULL, 10);

		off_t total = 0;
		char buffer[kPCloudBufferSize];
		if (chunked) {
			while (true) {
				BString line;
				while (true) {
					ssize_t read = socket.Read(&character, 1);
					if (read <= 0) {
						_SetError("truncated chunked response");
						return B_BAD_DATA;
					}
					if (character == '\n')
						break;
					if (character != '\r')
						line.Append(&character, 1);
				}
				char* end = NULL;
				long chunkSize = strtol(line.String(), &end, 16);
				if (end == NULL || *end != '\0' || chunkSize < 0) {
					_SetError("invalid chunk size: %s", line.String());
					return B_BAD_DATA;
				}
				if (chunkSize == 0)
					break;
				while (chunkSize > 0) {
					size_t wanted = chunkSize < (long)sizeof(buffer)
						? (size_t)chunkSize : sizeof(buffer);
					ssize_t read = socket.Read(buffer, wanted);
					if (read <= 0) {
						_SetError("truncated chunked response body");
						return B_BAD_DATA;
					}
					status = _Deliver(buffer, (size_t)read, body, fd,
						fileOffset + total, hasher);
					if (status != B_OK)
						return status;
					total += read;
					chunkSize -= read;
				}
				socket.Read(&character, 1);
				socket.Read(&character, 1);
			}
		} else {
			while (contentLength < 0 || total < contentLength) {
				size_t wanted = sizeof(buffer);
				if (contentLength >= 0
					&& contentLength - total < (off_t)wanted) {
					wanted = (size_t)(contentLength - total);
				}
				if (wanted == 0)
					break;
				ssize_t read = socket.Read(buffer, wanted);
				if (read < 0) {
					_SetError("reading response failed: %s", strerror((int)read));
					return (status_t)read;
				}
				if (read == 0)
					break;
				status = _Deliver(buffer, (size_t)read, body, fd,
					fileOffset + total, hasher);
				if (status != B_OK)
					return status;
				total += read;
			}
		}

		if (body.Length() > kPCloudMaxResponseSize) {
			_SetError("response too large (%ld bytes)", (long)total);
			return B_BAD_DATA;
		}
		return B_OK;
	}

	_SetError("too many redirects");
	return B_BAD_DATA;
}
