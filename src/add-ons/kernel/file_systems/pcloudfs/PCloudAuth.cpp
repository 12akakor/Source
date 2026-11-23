/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "PCloudAuth.h"

#include "PCloudDefs.h"
#include "PCloudJson.h"
#include "PCloudLogger.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <new>

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Url.h>


static status_t
flatten_message(const BMessage& message, char*& buffer, ssize_t& size)
{
	size = message.FlattenedSize();
	if (size <= 0)
		return B_BAD_VALUE;
	buffer = new(std::nothrow) char[size];
	if (buffer == NULL)
		return B_NO_MEMORY;
	return message.Flatten(buffer, size);
}


PCloudAuth::PCloudAuth()
	:
	fHttp(),
	fSettingsPath(),
	fUsername(),
	fPassword(),
	fAccessToken(),
	fUid(),
	fLastError()
{
}


status_t
PCloudAuth::Init()
{
	status_t status = _Load();
	if (status == B_OK || status == B_ENTRY_NOT_FOUND)
		return B_OK;
	return status;
}


bool
PCloudAuth::HasToken() const
{
	return fAccessToken.Length() > 0;
}


bool
PCloudAuth::HasCredentials() const
{
	return fUsername.Length() > 0 && fPassword.Length() > 0;
}


BString
PCloudAuth::AuthQuery() const
{
	BString query;
	query.SetToFormat("auth=%s", BUrl::UrlEncode(fAccessToken).String());
	return query;
}


status_t
PCloudAuth::LoginWithPassword(const BString& username,
	const BString& password)
{
	if (username.Length() == 0 || password.Length() == 0)
		return B_BAD_VALUE;

	BString parameters;
	parameters.SetToFormat("getauth=1&logout=1&timeformat=timestamp"
		"&username=%s&password=%s", BUrl::UrlEncode(username).String(),
		BUrl::UrlEncode(password).String());

	BString url;
	url.SetToFormat("https://%s/userinfo?%s", kPCloudApiHost,
		parameters.String());

	BString response;
	status_t status = fHttp.Get(url, "", response);
	if (status != B_OK) {
		fLastError = fHttp.LastError();
		return status;
	}

	BMessage json;
	status = PCloudJson::Parse(response, json);
	if (status != B_OK) {
		fLastError = "pCloud returned an unreadable reply";
		return B_BAD_DATA;
	}

	int64 result = 0;
	if (!PCloudJson::GetInt64(json, "result", result)) {
		fLastError = "pCloud reply has no result field";
		return B_BAD_DATA;
	}
	if (result != 0) {
		BString error;
		PCloudJson::GetString(json, "error", error);
		if (result == 1000 || result == 2000) {
			fLastError = "pCloud rejected the login. Check your e-mail and "
				"password. Accounts with two-factor authentication may not "
				"be supported by pCloud's password API.";
		} else {
			fLastError.SetToFormat("pCloud error %" B_PRId64 ": %s", result,
				error.Length() > 0 ? error.String() : "unknown");
		}
		return B_ERROR;
	}

	BString token;
	int64 uid = 0;
	if (!PCloudJson::GetString(json, "auth", token)) {
		fLastError = "pCloud login reply has no auth token";
		return B_BAD_DATA;
	}
	PCloudJson::GetInt64(json, "uid", uid);

	fAccessToken = token;
	fUsername = username;
	fPassword = password;
	if (uid != 0)
		fUid.SetToFormat("%" B_PRId64, uid);

	status = _Save();
	if (status != B_OK)
		PCLOUD_LOG_WARNING("could not save login: %s", strerror(status));
	fLastError = "";
	return B_OK;
}


status_t
PCloudAuth::_Load()
{
	BPath path;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;
	if (path.Append("pcloudfs") != B_OK || path.Append("settings") != B_OK)
		return B_ERROR;

	BFile file(path.Path(), B_READ_ONLY);
	status = file.InitCheck();
	if (status != B_OK)
		return status;

	off_t size = 0;
	status = file.GetSize(&size);
	if (status != B_OK || size <= 0 || size > 1024 * 1024)
		return B_BAD_DATA;

	char* buffer = new(std::nothrow) char[size + 1];
	if (buffer == NULL)
		return B_NO_MEMORY;
	ssize_t bytes = file.Read(buffer, size);
	if (bytes < 0) {
		delete[] buffer;
		return (status_t)bytes;
	}
	buffer[bytes] = '\0';

	BMessage settings;
	status = settings.Unflatten(buffer);
	delete[] buffer;
	if (status != B_OK)
		return status;

	PCloudJson::GetString(settings, kSettingUsername, fUsername);
	PCloudJson::GetString(settings, kSettingPassword, fPassword);
	PCloudJson::GetString(settings, kSettingAccessToken, fAccessToken);
	PCloudJson::GetString(settings, kSettingUid, fUid);
	fSettingsPath = path.Path();
	return B_OK;
}


status_t
PCloudAuth::_Save()
{
	if (fSettingsPath.Length() == 0) {
		BPath path;
		status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
		if (status != B_OK)
			return status;
		if (path.Append("pcloudfs") != B_OK)
			return B_ERROR;
		BDirectory directory;
		status = directory.SetTo(path.Path());
		if (status == B_ENTRY_NOT_FOUND) {
			status = create_directory(path.Path(), 0700);
			if (status == B_OK)
				status = directory.SetTo(path.Path());
		}
		if (status != B_OK)
			return status;
		if (path.Append("settings") != B_OK)
			return B_ERROR;
		fSettingsPath = path.Path();
	}

	BMessage settings;
	settings.AddString(kSettingAuthType, "password");
	settings.AddString(kSettingUsername, fUsername.String());
	settings.AddString(kSettingPassword, fPassword.String());
	settings.AddString(kSettingAccessToken, fAccessToken.String());
	settings.AddString(kSettingUid, fUid.String());

	char* buffer = NULL;
	ssize_t size = 0;
	status_t status = flatten_message(settings, buffer, size);
	if (status != B_OK)
		return status;

	BFile file(fSettingsPath.String(),
		B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
	status = file.InitCheck();
	if (status == B_OK) {
		status = file.SetPermissions(0600);
		if (status == B_OK)
			status = file.Write(buffer, size) == size ? B_OK : B_IO_ERROR;
		if (status == B_OK)
			status = file.Sync();
	}
	delete[] buffer;
	return status;
}
