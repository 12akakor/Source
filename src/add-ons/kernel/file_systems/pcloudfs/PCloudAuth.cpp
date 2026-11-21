/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudAuth.h"

#include "PCloudCrypto.h"
#include "PCloudDefs.h"
#include "PCloudJson.h"
#include "PCloudLogger.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
PCloudAuth::IsConfigured() const
{
	return fClientId.Length() > 0 && fClientSecret.Length() > 0;
}


bool
PCloudAuth::HasToken() const
{
	return fAccessToken.Length() > 0;
}


status_t
PCloudAuth::Configure(const BString& clientId, const BString& clientSecret,
	const BString& redirectUri)
{
	fClientId = clientId;
	fClientSecret = clientSecret;
	fRedirectUri = redirectUri;
	return _Save();
}


BString
PCloudAuth::AuthorizeUrl(const BString& state,
	const BString& codeChallenge) const
{
	BString url;
	url.SetToFormat("https://www.pcloud.com/oauth/authorize"
		"?client_id=%s&response_type=code&redirect_uri=%s",
		BUrl::UrlEncode(fClientId).String(),
		BUrl::UrlEncode(fRedirectUri).String());
	if (state.Length() > 0) {
		url << "&state=" << BUrl::UrlEncode(state);
	}
	if (codeChallenge.Length() > 0) {
		url << "&code_challenge=" << BUrl::UrlEncode(codeChallenge);
		url << "&code_challenge_method=S256";
	}
	return url;
}


status_t
PCloudAuth::ExchangeCode(const BString& code)
{
	BString body;
	body.SetToFormat("client_id=%s&client_secret=%s&code=%s"
		"&redirect_uri=%s&grant_type=authorization_code",
		BUrl::UrlEncode(fClientId).String(),
		BUrl::UrlEncode(fClientSecret).String(),
		BUrl::UrlEncode(code).String(),
		BUrl::UrlEncode(fRedirectUri).String());
	return _TokenRequest(body);
}


status_t
PCloudAuth::RefreshToken()
{
	if (fRefreshToken.Length() == 0)
		return B_BAD_VALUE;
	BString body;
	body.SetToFormat("client_id=%s&client_secret=%s&refresh_token=%s"
		"&grant_type=refresh_token",
		BUrl::UrlEncode(fClientId).String(),
		BUrl::UrlEncode(fClientSecret).String(),
		BUrl::UrlEncode(fRefreshToken).String());
	return _TokenRequest(body);
}


BString
PCloudAuth::GenerateVerifier()
{
	uint8 bytes[32];
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		if (read(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
			close(fd);
			return BString();
		}
		close(fd);
	} else {
		return BString();
	}
	return PCloudBase64Url::Encode(bytes, sizeof(bytes));
}


BString
PCloudAuth::ChallengeForVerifier(const BString& verifier)
{
	uint8 digest[32];
	PCloudSHA256::Hash(verifier.String(), verifier.Length(), digest);
	return PCloudBase64Url::Encode(digest, sizeof(digest));
}


status_t
PCloudAuth::_TokenRequest(const BString& body)
{
	BString response;
	status_t status = fHttp.PostForm("/oauth2/token", body, "", response);
	if (status != B_OK) {
		fLastError = fHttp.LastError();
		return status;
	}

	BMessage json;
	status = PCloudJson::Parse(response, json);
	if (status != B_OK) {
		fLastError.SetToFormat("invalid token response: %s",
			fHttp.LastError().String());
		return B_BAD_DATA;
	}

	int64 result = 0;
	if (!PCloudJson::GetInt64(json, "result", result) || result != 0) {
		BString error;
		PCloudJson::GetString(json, "error", error);
		fLastError.SetToFormat("pCloud error %" B_PRId64 ": %s", result,
			error.Length() > 0 ? error.String() : "unknown");
		return B_ERROR;
	}

	BString accessToken;
	BString refreshToken;
	BString uid;
	int64 uidNumber = 0;
	if (!PCloudJson::GetString(json, "access_token", accessToken)) {
		// Older pCloud endpoints return the token in "auth".
		PCloudJson::GetString(json, "auth", accessToken);
	}
	PCloudJson::GetString(json, "refresh_token", refreshToken);
	if (PCloudJson::GetInt64(json, "uid", uidNumber))
		uid.SetToFormat("%" B_PRId64, uidNumber);
	PCloudJson::GetString(json, "uid", uid);

	if (accessToken.Length() == 0) {
		fLastError = "token response has no access token";
		return B_BAD_DATA;
	}

	fAccessToken = accessToken;
	if (refreshToken.Length() > 0)
		fRefreshToken = refreshToken;
	if (uid.Length() > 0)
		fUid = uid;

	status = _Save();
	if (status != B_OK)
		PCLOUD_LOG_WARNING("could not save OAuth2 token: %s", strerror(status));
	return B_OK;
}


status_t
PCloudAuth::_Load()
{
	BPath path;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;
	if (path.Append("pcloudfs") != B_OK)
		return B_ERROR;
	if (path.Append("settings") != B_OK)
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

	PCloudJson::GetString(settings, kSettingClientId, fClientId);
	PCloudJson::GetString(settings, kSettingClientSecret, fClientSecret);
	PCloudJson::GetString(settings, kSettingRedirectUri, fRedirectUri);
	PCloudJson::GetString(settings, kSettingAccessToken, fAccessToken);
	PCloudJson::GetString(settings, kSettingRefreshToken, fRefreshToken);
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
			if (status != B_OK)
				return status;
			status = directory.SetTo(path.Path());
		}
		if (status != B_OK)
			return status;
		if (path.Append("settings") != B_OK)
			return B_ERROR;
		fSettingsPath = path.Path();
	}

	BMessage settings;
	settings.AddString(kSettingClientId, fClientId.String());
	settings.AddString(kSettingClientSecret, fClientSecret.String());
	settings.AddString(kSettingRedirectUri, fRedirectUri.String());
	settings.AddString(kSettingAccessToken, fAccessToken.String());
	settings.AddString(kSettingRefreshToken, fRefreshToken.String());
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
