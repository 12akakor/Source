/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudApi.h"

#include "PCloudDefs.h"
#include "PCloudJson.h"
#include "PCloudLogger.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <new>

#include <fcntl.h>
#include <unistd.h>

#include <Entry.h>
#include <Url.h>


static int64
epoch_from_utc_tm(const struct tm& t)
{
	// Howard Hinnant's days-from-civil algorithm; no timegm() needed.
	int64 year = (int64)t.tm_year + 1900;
	int64 month = (int64)t.tm_mon + 1;
	if (month <= 2)
		year--;
	int64 era = (year >= 0 ? year : year - 399) / 400;
	int64 yearOfEra = year - era * 400;
	int64 dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5
		+ (int64)t.tm_mday - 1;
	int64 dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100
		+ dayOfYear;
	int64 days = era * 146097 + dayOfEra - 719468;
	return days * 86400 + (int64)t.tm_hour * 3600 + (int64)t.tm_min * 60
		+ (int64)t.tm_sec;
}


static status_t
parse_pcloud_date(const BString& text, int64& result)
{
	if (text.Length() == 0)
		return B_BAD_VALUE;
	struct tm time;
	memset(&time, 0, sizeof(time));
	const char* end = strptime(text.String(), "%a, %d %b %Y %H:%M:%S %z",
		&time);
	if (end == NULL)
		return B_BAD_VALUE;
	result = epoch_from_utc_tm(time);
	return B_OK;
}


static bool
node_from_metadata(const BMessage& metadata, PCloudNode& node)
{
	int64 folderId = 0;
	int64 fileId = 0;
	int64 parentId = 0;
	int64 size = 0;
	bool isFolder = false;
	BString name;
	BString modified;

	if (!PCloudJson::GetBool(metadata, "isfolder", isFolder))
		return false;
	if (!PCloudJson::GetString(metadata, "name", name))
		return false;
	if (isFolder) {
		if (!PCloudJson::GetInt64(metadata, "folderid", folderId))
			return false;
		node.fId.SetToFormat("%" B_PRId64, folderId);
	} else {
		if (!PCloudJson::GetInt64(metadata, "fileid", fileId))
			return false;
		node.fId.SetToFormat("%" B_PRId64, fileId);
		PCloudJson::GetInt64(metadata, "size", size);
	}
	if (PCloudJson::GetInt64(metadata, "parentfolderid", parentId))
		node.fParentId.SetToFormat("%" B_PRId64, parentId);
	PCloudJson::GetString(metadata, "modified", modified);

	node.fName = name;
	node.fIsFolder = isFolder;
	node.fSize = size;
	node.fRefreshed = system_time();
	if (parse_pcloud_date(modified, node.fModified) != B_OK)
		node.fModified = 0;
	return true;
}


PCloudApi::PCloudApi(PCloudAuth& auth)
	:
	fAuth(auth),
	fHttp(),
	fLastError()
{
}


status_t
PCloudApi::UserInfo(BString& email, off_t& quota, off_t& used)
{
	BMessage reply;
	status_t status = _Call("userinfo", "", reply);
	if (status != B_OK)
		return status;

	int64 quotaValue = 0;
	int64 usedValue = 0;
	PCloudJson::GetString(reply, "email", email);
	PCloudJson::GetInt64(reply, "quota", quotaValue);
	PCloudJson::GetInt64(reply, "used", usedValue);
	quota = (off_t)quotaValue;
	used = (off_t)usedValue;
	return B_OK;
}


status_t
PCloudApi::ListFolder(const BString& folderId, BObjectList<PCloudNode>& nodes)
{
	BString parameters;
	parameters.SetToFormat("folderid=%s", BUrl::UrlEncode(folderId).String());

	BMessage reply;
	status_t status = _Call("listfolder", parameters.String(), reply);
	if (status != B_OK)
		return status;

	BMessage metadata;
	if (reply.FindMessage("metadata", &metadata) != B_OK) {
		_SetError("listfolder reply has no metadata");
		return B_BAD_DATA;
	}

	BMessage contents;
	if (metadata.FindMessage("contents", &contents) != B_OK) {
		_SetError("listfolder reply has no contents");
		return B_BAD_DATA;
	}

	int32 count = PCloudJson::ArrayCount(contents);
	for (int32 i = 0; i < count; i++) {
		BMessage item;
		if (PCloudJson::ArrayItem(contents, i, item) != B_OK)
			continue;
		PCloudNode* node = new(std::nothrow) PCloudNode;
		if (node == NULL)
			return B_NO_MEMORY;
		if (!node_from_metadata(item, *node)) {
			delete node;
			continue;
		}
		if (!nodes.AddItem(node)) {
			delete node;
			return B_NO_MEMORY;
		}
	}
	return B_OK;
}


status_t
PCloudApi::Checksum(const BString& fileId, BString& sha1, off_t& size)
{
	BString parameters;
	parameters.SetToFormat("fileid=%s", BUrl::UrlEncode(fileId).String());

	BMessage reply;
	status_t status = _Call("checksumfile", parameters.String(), reply);
	if (status != B_OK)
		return status;
	if (!PCloudJson::GetString(reply, "sha1", sha1)) {
		_SetError("checksumfile reply has no sha1");
		return B_BAD_DATA;
	}
	sha1.ToLower();

	BMessage metadata;
	if (reply.FindMessage("metadata", &metadata) == B_OK) {
		int64 value = 0;
		if (PCloudJson::GetInt64(metadata, "size", value))
			size = (off_t)value;
	}
	return B_OK;
}


status_t
PCloudApi::Download(const BString& fileId, const BString& destinationPath,
	PCloudSHA1& hash)
{
	BString parameters;
	parameters.SetToFormat("fileid=%s&forcedownload=1",
		BUrl::UrlEncode(fileId).String());

	BMessage reply;
	status_t status = _Call("getfilelink", parameters.String(), reply);
	if (status != B_OK)
		return status;

	BString path;
	if (!PCloudJson::GetString(reply, "path", path)) {
		_SetError("getfilelink reply has no path");
		return B_BAD_DATA;
	}
	BMessage hosts;
	if (reply.FindMessage("hosts", &hosts) != B_OK
		|| PCloudJson::ArrayCount(hosts) < 1) {
		_SetError("getfilelink reply has no hosts");
		return B_BAD_DATA;
	}
	BString host;
		// Hosts are an array of strings, not of messages: read field "0".
	if (hosts.FindString("0", &host) != B_OK) {
		_SetError("getfilelink reply has an invalid host");
		return B_BAD_DATA;
	}

	BString url;
	url.SetToFormat("https://%s%s", host.String(), path.String());

	int fd = open(destinationPath.String(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		_SetError("cannot create %s: %s", destinationPath.String(),
			strerror(errno));
		return (status_t)errno;
	}

	status = fHttp.DownloadToFile(url, "", fd, 0, hash);
	if (status != B_OK) {
		fLastError = fHttp.LastError();
		close(fd);
		unlink(destinationPath.String());
		return status;
	}
	if (fsync(fd) != 0) {
		_SetError("cannot flush %s: %s", destinationPath.String(),
			strerror(errno));
		close(fd);
		return (status_t)errno;
	}
	close(fd);
	return B_OK;
}


void
PCloudApi::_SetError(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	fLastError.SetToFormatVarArgs(format, args);
	va_end(args);
	PCLOUD_LOG_ERROR("API: %s", fLastError.String());
}


status_t
PCloudApi::_Call(const char* method, const char* parameters, BMessage& reply)
{
	if (!fAuth.HasToken()) {
		_SetError("no access token; run pcloudctl setup first");
		return B_PERMISSION_DENIED;
	}

	BString url;
	url.SetToFormat("https://%s/%s", kPCloudApiHost, method);
	if (parameters != NULL && parameters[0] != '\0') {
		url << '?' << parameters;
	}

	BString body;
	status_t status = fHttp.Get(url, fAuth.AccessToken(), body);
	if (status != B_OK) {
		fLastError = fHttp.LastError();
		return status;
	}

	status = PCloudJson::Parse(body, reply);
	if (status != B_OK) {
		_SetError("invalid JSON from %s", method);
		return B_BAD_DATA;
	}

	int64 result = 0;
	if (!PCloudJson::GetInt64(reply, "result", result)) {
		_SetError("%s reply has no result", method);
		return B_BAD_DATA;
	}
	if (result != 0) {
		BString error;
		PCloudJson::GetString(reply, "error", error);
		_SetError("%s failed: %" B_PRId64 " (%s)", method, result,
			error.Length() > 0 ? error.String() : "no message");
		return B_ERROR;
	}
	return B_OK;
}
