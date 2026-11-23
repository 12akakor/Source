/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_AUTH_H
#define _PCLOUD_AUTH_H


#include "PCloudHttp.h"

#include <Message.h>
#include <String.h>


/*! pCloud password authentication and token bookkeeping.

	pCloud has no /login method. Login is performed by calling userinfo with
	the global username/password parameters and getauth=1; the reply contains
	an `auth` token. That token is sent as the `auth` query parameter on all
	following calls (OAuth2 Bearer tokens are no longer used by this client).

	Credentials are stored in a 0600 settings file. A future iteration should
	move the password into Haiku's BKeyStore; the auth-type field is already
	present so that the token format can be recognised.
*/
class PCloudAuth {
public:
						PCloudAuth();

		status_t		Init();

		bool			HasToken() const;
		bool			HasCredentials() const;
		bool			UsesAuthParameter() const	{ return true; }

		const BString&	AccessToken() const	{ return fAccessToken; }
		const BString&	Uid() const			{ return fUid; }
		const BString&	Username() const	{ return fUsername; }
		const BString&	LastError() const	{ return fLastError; }

	/*! Returns "auth=<urlencoded token>" for the query string. */
		BString			AuthQuery() const;

		status_t		LoginWithPassword(const BString& username,
							const BString& password);

private:
		status_t		_Load();
		status_t		_Save();

private:
			PCloudHttp	fHttp;
			BString		fSettingsPath;
			BString		fUsername;
			BString		fPassword;
			BString		fAccessToken;
			BString		fUid;
			BString		fLastError;
};


#endif	// _PCLOUD_AUTH_H
