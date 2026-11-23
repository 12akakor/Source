/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "PCloudApi.h"
#include "PCloudAuth.h"
#include "PCloudCache.h"
#include "PCloudCrypto.h"
#include "PCloudDefs.h"
#include "PCloudJournal.h"
#include "PCloudJson.h"
#include "PCloudLogger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Path.h>
#include <String.h>


static void
print_usage(const char* program)
{
	fprintf(stderr,
		"Usage: %s <command>\n"
		"\n"
		"  login                 Log in with pCloud e-mail and password\n"
		"  status                Show account, token and cache state\n"
		"  selftest              Run local integrity and parser tests\n"
		"\n"
		"Mount after login with:\n"
		"  mkdir /pCloud\n"
		"  mount -t userlandfs -p pcloudfs /pCloud\n",
		program);
}


static BString
read_line(const char* prompt)
{
	char buffer[1024];
	printf("%s", prompt);
	fflush(stdout);
	if (fgets(buffer, sizeof(buffer), stdin) == NULL)
		return BString();
	BString result(buffer);
	result.Trim();
	return result;
}


static status_t
login(PCloudAuth& auth)
{
	BString username = read_line("pCloud e-mail: ");
	BString password = read_line("Password: ");
	if (username.Length() == 0 || password.Length() == 0) {
		fprintf(stderr, "E-mail and password are required.\n");
		return B_BAD_VALUE;
	}

	status_t status = auth.LoginWithPassword(username, password);
	if (status != B_OK) {
		fprintf(stderr, "Login failed: %s\n", auth.LastError().String());
		return status;
	}
	printf("Logged in as pCloud user %s.\n", auth.Uid().String());
	return B_OK;
}


static status_t
show_status(PCloudAuth& auth)
{
	printf("Username      : %s\n", auth.Username().Length() > 0
		? auth.Username().String() : "-");
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
	printf("%-56s %s\n", description, condition ? "ok" : "FAILED");
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
		== "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0",
		"SHA-256 / base64url of abc");

	allOk &= check(PCloudCrypto::Crc32C("123456789", 9) == 0xe3069283,
		"CRC32C check vector");

	BMessage json;
	status_t status = PCloudJson::Parse(
		"{\"result\":0,\"auth\":\"TOKEN\",\"metadata\":{\"contents\":["
		"{\"fileid\":42,\"name\":\"a.txt\",\"isfolder\":false}"
		"]}}", json);
	allOk &= check(status == B_OK, "JSON parse");
	if (status == B_OK) {
		BString token;
		allOk &= check(PCloudJson::GetString(json, "auth", token)
			&& token == "TOKEN", "auth token parsed");
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

	if (strcmp(argv[1], "login") == 0)
		status = login(auth);
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
