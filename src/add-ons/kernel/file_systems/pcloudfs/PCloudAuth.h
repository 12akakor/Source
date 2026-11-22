/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_AUTH_H
#define _PCLOUD_AUTH_H


#include "PCloudHttp.h"

#include <Message.h>
#include <String.h>


/*! pCloud OAuth2 credentials and token bookkeeping.

	Settings live in B_USER_SETTINGS_DIRECTORY/pcloudfs/settings as a flattened
	BMessage. The access token is held in memory and used as a Bearer token by
	PCloudApi. Refreshing is explicit so a failing refresh is visible in the
	syslog instead of causing an opaque EACCES in the filesystem.
*/
class PCloudAuth {
public:
						PCloudAuth();

		status_t		Init();
		bool			IsConfigured() const;
		bool			HasToken() const;

		const BString&	AccessToken() const	{ return fAccessToken; }
		bool			HasRefreshToken() const
						{ return fRefreshToken.Length() > 0; }
		const BString&	Uid() const			{ return fUid; }
		const BString&	LastError() const	{ return fLastError; }

		status_t		Configure(const BString& clientId,
							const BString& clientSecret,
							const BString& redirectUri);

		BString			AuthorizeUrl(const BString& state) const;

		status_t		ExchangeCode(const BString& code);
		status_t		RefreshToken();

		static BString	GenerateVerifier();
		static BString	ChallengeForVerifier(const BString& verifier);

private:
		status_t		_Load();
		status_t		_Save();
		status_t		_TokenRequest(const BString& body);

private:
			PCloudHttp	fHttp;
			BString		fSettingsPath;
			BString		fClientId;
			BString		fClientSecret;
			BString		fRedirectUri;
			BString		fAccessToken;
			BString		fRefreshToken;
			BString		fUid;
			BString		fLastError;
};


#endif	// _PCLOUD_AUTH_H
