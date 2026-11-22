/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudApi.h"
#include "PCloudAuth.h"
#include "PCloudCache.h"
#include "PCloudCrypto.h"
#include "PCloudDefs.h"
#include "PCloudJson.h"
#include "PCloudLogger.h"
#include "PCloudNode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <NetworkAddress.h>
#include <Socket.h>
#include <Url.h>


static const char kDefaultRedirectUri[] = "http://localhost:8765/callback";


static void
print_usage(const char* program)
{
	fprintf(stderr,
		"Usage: %s <command>\n"
		"\n"
		"  auth-url              Print the authorization URL (use from another PC)\n"
		"  setup [code]           Authorize; paste the code if browser is elsewhere\n"
		"  status                Show configuration, token and cache state\n"
		"  selftest              Run local integrity/parser self tests\n"
		"\n"
		"Mount after setup with:\n"
		"  mkdir /pCloud\n"
		"  mount -t userlandfs -p pcloudfs /pCloud\n",
		program);
}


static status_t
wait_for_code(BString& code)
{
	BNetworkAddress local("127.0.0.1", 8765);
	status_t status = local.InitCheck();
	if (status != B_OK)
		return status;

	BSocket listener;
	status = listener.Bind(local);
	if (status != B_OK)
		return status;
	status = listener.Listen(1);
	if (status != B_OK)
		return status;

	printf("Waiting for the browser redirect on %s ...\n", kDefaultRedirectUri);
	BAbstractSocket* connection = NULL;
	status = listener.Accept(connection);
	if (status != B_OK)
		return status;

	char request[8192];
	ssize_t bytes = connection->Read(request, sizeof(request) - 1);
	if (bytes > 0)
		request[bytes] = '\0';
	else
		request[0] = '\0';

	const char* response = "HTTP/1.1 200 OK\r\n"
		"Content-Type: text/html\r\n"
		"Connection: close\r\n"
		"\r\n"
		"<html><body><h1>pcloudfs</h1>"
		"<p>Authorization received. You can close this window.</p>"
		"</body></html>\r\n";
	connection->Write(response, strlen(response));
	delete connection;

	BString requestString(request);
	int32 codeStart = requestString.FindFirst("code=");
	if (codeStart < 0)
		return B_BAD_VALUE;
	codeStart += 5;
	int32 codeEnd = requestString.FindFirst("&", codeStart);
	if (codeEnd < 0)
		codeEnd = requestString.FindFirst(" ", codeStart);
	if (codeEnd < 0)
		codeEnd = requestString.Length();
	code.SetTo(requestString.String() + codeStart, codeEnd - codeStart);
	code = BUrl::UrlDecode(code);
	return code.Length() > 0 ? B_OK : B_BAD_VALUE;
}


static status_t
ensure_credentials(PCloudAuth& auth)
{
	if (auth.IsConfigured())
		return B_OK;

	BString clientId;
	BString clientSecret;
	printf("pCloud OAuth2 setup\n");
	printf("Client ID: ");
	char line[1024];
	if (fgets(line, sizeof(line), stdin) == NULL)
		return B_BAD_VALUE;
	clientId.SetTo(line);
	clientId.Trim();
	printf("Client secret: ");
	if (fgets(line, sizeof(line), stdin) == NULL)
		return B_BAD_VALUE;
	clientSecret.SetTo(line);
	clientSecret.Trim();
	if (clientId.Length() == 0 || clientSecret.Length() == 0)
		return B_BAD_VALUE;
	return auth.Configure(clientId, clientSecret, kDefaultRedirectUri);
}


static status_t
print_authorize_url(PCloudAuth& auth)
{
	status_t status = ensure_credentials(auth);
	if (status != B_OK)
		return status;

	BString state = PCloudAuth::GenerateVerifier();
	BString url = auth.AuthorizeUrl(state);
	printf("\nOpen this URL in a browser and approve the app:\n\n%s\n\n",
		url.String());
	printf("Copy the code shown by pCloud, then run:\n");
	printf("  pcloudctl setup <code>\n\n");
	return B_OK;
}


static status_t
setup(PCloudAuth& auth, const char* codeArgument)
{
	status_t status = ensure_credentials(auth);
	if (status != B_OK)
		return status;

	BString code;
	if (codeArgument != NULL) {
		code.SetTo(codeArgument);
		code.Trim();
	} else {
		BString state = PCloudAuth::GenerateVerifier();
		BString url = auth.AuthorizeUrl(state);
		printf("\nOpen this URL in a browser and approve the app:\n\n%s\n\n",
			url.String());
		status = wait_for_code(code);
		if (status != B_OK) {
			fprintf(stderr, "Could not read the authorization code: %s\n",
				strerror(status));
			return status;
		}
	}

	status = auth.ExchangeCode(code);
	if (status != B_OK) {
		fprintf(stderr, "Token exchange failed: %s\n",
			auth.LastError().String());
		return status;
	}
	printf("Authorization complete. UID: %s\n", auth.Uid().String());
	return B_OK;
}


static status_t
show_status(PCloudAuth& auth)
{
	printf("Configuration : %s\n", auth.IsConfigured() ? "yes" : "no");
	printf("Access token  : %s\n", auth.HasToken() ? "present" : "missing");
	printf("UID           : %s\n", auth.Uid().Length() > 0
		? auth.Uid().String() : "-");

	PCloudCache cache;
	status_t status = cache.Init(auth.Uid().Length() > 0 ? auth.Uid() : "0");
	if (status != B_OK) {
		fprintf(stderr, "Cache error: %s\n", strerror(status));
		return status;
	}
	printf("Cache         : %s\n", cache.Root().String());

	if (auth.HasToken()) {
		PCloudApi api(auth);
		BString email;
		off_t quota = 0;
		off_t used = 0;
		status = api.UserInfo(email, quota, used);
		if (status == B_OK) {
			printf("Account       : %s\n", email.String());
			printf("Quota         : %ld / %ld bytes\n", (long)used,
				(long)quota);
		} else {
			fprintf(stderr, "pCloud status failed: %s\n",
				api.LastError().String());
		}
	}
	return B_OK;
}


static bool
check(bool condition, const char* description)
{
	printf("%-52s %s\n", description, condition ? "ok" : "FAILED");
	return condition;
}


static status_t
selftest()
{
	bool allOk = true;

	uint8 sha1[20];
	PCloudSHA1::Hash("abc", 3, sha1);
	allOk &= check(PCloudSHA1::ToHex(sha1)
		== "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of abc");

	uint8 sha256[32];
	PCloudSHA256::Hash("abc", 3, sha256);
	allOk &= check(PCloudBase64Url::Encode(sha256, sizeof(sha256))
		== "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qct_BOIDjkK6c",
		"SHA-256 / base64url of abc");

	BMessage json;
	status_t status = PCloudJson::Parse(
		"{\"result\":0,\"metadata\":{\"contents\":["
		"{\"fileid\":42,\"name\":\"a.txt\",\"isfolder\":false}"
		"]}}", json);
	allOk &= check(status == B_OK, "JSON parse");
	if (status == B_OK) {
		BMessage metadata;
		BMessage contents;
		BMessage item;
		int64 fileId = 0;
		BString name;
		bool ok = json.FindMessage("metadata", &metadata) == B_OK
			&& metadata.FindMessage("contents", &contents) == B_OK
			&& PCloudJson::ArrayCount(contents) == 1
			&& PCloudJson::ArrayItem(contents, 0, item) == B_OK
			&& PCloudJson::GetInt64(item, "fileid", fileId) && fileId == 42
			&& PCloudJson::GetString(item, "name", name) && name == "a.txt";
		allOk &= check(ok, "JSON nested objects and arrays");
	}

	PCloudCache cache;
	status = cache.Init("selftest");
	allOk &= check(status == B_OK, "cache directories");
	if (status == B_OK) {
		PCloudNode node;
		node.fId = "selftest-node";
		node.fName = "node.txt";
		node.fParentId = "0";
		node.fSize = 3;
		status = cache.PutNode(node);
		allOk &= check(status == B_OK, "cache metadata write");
		PCloudNode loaded;
		status = cache.GetNode(node.fId, loaded);
		allOk &= check(status == B_OK && loaded.fName == node.fName
			&& loaded.fSize == 3, "cache metadata read");
	}

	return allOk ? B_OK : B_ERROR;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}
	gPCloudLogger.Init("");

	PCloudAuth auth;
	status_t status = auth.Init();
	if (status != B_OK) {
		fprintf(stderr, "Cannot read settings: %s\n", strerror(status));
		return 1;
	}

	if (strcmp(argv[1], "setup") == 0)
		status = setup(auth, argc > 2 ? argv[2] : NULL);
	else if (strcmp(argv[1], "auth-url") == 0)
		status = print_authorize_url(auth);
	else if (strcmp(argv[1], "status") == 0)
		status = show_status(auth);
	else if (strcmp(argv[1], "selftest") == 0)
		status = selftest();
	else {
		print_usage(argv[0]);
		return 1;
	}

	if (status != B_OK) {
		fprintf(stderr, "%s failed (status %ld)\n", argv[1], (long)status);
		return 1;
	}
	return 0;
}
