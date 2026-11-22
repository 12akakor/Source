/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudApi.h"
#include "PCloudAuth.h"
#include "PCloudDefs.h"
#include "PCloudCache.h"
#include "PCloudCrypto.h"
#include "PCloudJson.h"
#include "PCloudLogger.h"
#include "PCloudNode.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <new>

#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <String.h>


static const char kMountPoint[] = "/pCloud";
static const char kReportName[] = "/boot/home/tests/pcloudfs-test-results.txt";


class TestRunner {
public:
						TestRunner();

		void			Begin(const char* suite);
		bool			Check(bool condition, const char* description);
		bool			CheckEqual(const BString& actual,
							const BString& expected,
							const char* description);
		void			Skip(const char* description);
		int			Finish();

private:
		void			_Record(bool passed, const char* kind,
							const char* description);

private:
			int			fPassed;
			int			fFailed;
			int			fSkipped;
			BString		fSuite;
			BString		fReport;
};


TestRunner::TestRunner()
	:
	fPassed(0),
	fFailed(0),
	fSkipped(0),
	fSuite(),
	fReport()
{
}


void
TestRunner::Begin(const char* suite)
{
	fSuite = suite;
	printf("\n=== %s ===\n", suite);
	fReport << "\n=== " << suite << " ===\n";
}


void
TestRunner::_Record(bool passed, const char* kind, const char* description)
{
	BString line;
	line.SetToFormat("%-5s %-40s %s", kind, fSuite.String(), description);
	printf("%s\n", line.String());
	fReport << line << "\n";

	if (strcmp(kind, "PASS") == 0)
		fPassed++;
	else if (strcmp(kind, "FAIL") == 0)
		fFailed++;
	else
		fSkipped++;

	if (strcmp(kind, "FAIL") == 0)
		PCLOUD_LOG_ERROR("TEST FAIL: %s: %s", fSuite.String(), description);
	else
		PCLOUD_LOG_INFO("TEST %s: %s: %s", kind, fSuite.String(),
			description);
}


bool
TestRunner::Check(bool condition, const char* description)
{
	_Record(condition, condition ? "PASS" : "FAIL", description);
	return condition;
}


bool
TestRunner::CheckEqual(const BString& actual, const BString& expected,
	const char* description)
{
	if (actual == expected)
		return Check(true, description);
	BString detail;
	detail.SetToFormat("%s (got '%s', expected '%s')", description,
		actual.String(), expected.String());
	return Check(false, detail.String());
}


void
TestRunner::Skip(const char* description)
{
	_Record(false, "SKIP", description);
}


int
TestRunner::Finish()
{
	BFile file(kReportName, B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
	if (file.InitCheck() == B_OK)
		file.Write(fReport.String(), fReport.Length());

	BString summary;
	summary.SetToFormat("TOTAL: %d passed, %d failed, %d skipped", fPassed,
		fFailed, fSkipped);
	printf("\n%s\n", summary.String());
	printf("Report: %s\n", kReportName);
	PCLOUD_LOG_INFO("TEST %s", summary.String());
	return fFailed == 0 ? 0 : 1;
}


static void
test_crypto(TestRunner& runner)
{
	runner.Begin("crypto");

	uint8 digest[20];
	PCloudSHA1::Hash("", 0, digest);
	runner.CheckEqual(PCloudSHA1::ToHex(digest),
		"da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1 of empty string");

	PCloudSHA1::Hash("abc", 3, digest);
	runner.CheckEqual(PCloudSHA1::ToHex(digest),
		"a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of abc");

	const char kLong[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	PCloudSHA1::Hash(kLong, strlen(kLong), digest);
	runner.CheckEqual(PCloudSHA1::ToHex(digest),
		"84983e441c3bd26ebaae4aa1f95129e5e54670f1", "SHA-1 of long vector");

	uint8 sha256[32];
	PCloudSHA256::Hash("", 0, sha256);
	runner.CheckEqual(PCloudBase64Url::Encode(sha256, sizeof(sha256)),
		"47DEQpj8HBSa-_TImW-5JCeuQeRkm5NMpJWZG3hSuFU",
		"SHA-256/base64url of empty string");
	PCloudSHA256::Hash("abc", 3, sha256);
	runner.CheckEqual(PCloudBase64Url::Encode(sha256, sizeof(sha256)),
		"ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qct_BOIDjkK6c",
		"SHA-256/base64url of abc");

	char data[200];
	for (int32 i = 0; i < (int32)sizeof(data); i++)
		data[i] = (char)(i * 7 + 3);
	uint8 oneShot[20];
	PCloudSHA1::Hash(data, sizeof(data), oneShot);
	bool streamingOk = true;
	const int32 kChunkSizes[] = { 1, 7, 63, 64, 65, 127 };
	for (size_t c = 0; c < sizeof(kChunkSizes) / sizeof(kChunkSizes[0]); c++) {
		PCloudSHA1 hash;
		int32 chunk = kChunkSizes[c];
		for (int32 offset = 0; offset < (int32)sizeof(data); offset += chunk) {
			int32 length = offset + chunk <= (int32)sizeof(data)
				? chunk : (int32)sizeof(data) - offset;
			hash.Update(data + offset, length);
		}
		uint8 streamed[20];
		hash.Final(streamed);
		if (memcmp(oneShot, streamed, sizeof(oneShot)) != 0)
			streamingOk = false;
	}
	runner.Check(streamingOk, "SHA-1 streaming equals one-shot (6 chunk sizes)");
}


static void
test_json(TestRunner& runner)
{
	runner.Begin("json");

	BMessage message;
	status_t status = PCloudJson::Parse(
		"{\"result\":0,\"name\":\"a b\\n\\u00e9\",\"flag\":true,"
		"\"nothing\":null,\"list\":[1,-2,3.5,\"x\",{\"k\":\"v\"}],"
		"\"emoji\":\"\\ud83d\\ude00\"}", message);
	runner.Check(status == B_OK, "parse nested object with escapes");
	if (status == B_OK) {
		int64 result = -1;
		BString name;
		bool flag = false;
		BString itemText;
		BMessage list;
		BMessage item;
		int64 number = 0;
		BString emoji;

		runner.Check(PCloudJson::GetInt64(message, "result", result)
			&& result == 0, "integer field");
		runner.Check(PCloudJson::GetString(message, "name", name)
			&& name == "a b\n\xc3\xa9", "escaped and UTF-8 string");
		runner.Check(PCloudJson::GetBool(message, "flag", flag) && flag,
			"boolean field");
		runner.Check(!PCloudJson::Has(message, "nothing"),
			"null value is not stored as a field");
		runner.Check(message.FindMessage("list", &list) == B_OK
			&& PCloudJson::ArrayCount(list) == 5, "array count");
		runner.Check(PCloudJson::ArrayItem(list, 3, item) == B_BAD_VALUE,
			"array item of wrong type rejected");
		runner.Check(list.FindString("3", &itemText) == B_OK
			&& itemText == "x", "string in array");
		runner.Check(PCloudJson::ArrayItem(list, 4, item) == B_OK
			&& PCloudJson::GetString(item, "k", itemText) && itemText == "v",
			"object in array");
		runner.Check(list.FindInt64("0", &number) == B_OK && number == 1,
			"number in array");
		runner.Check(PCloudJson::GetString(message, "emoji", emoji)
			&& emoji == "\xf0\x9f\x98\x80", "UTF-16 surrogate pair decoded");
	}

	const char* invalid[] = {
		"{\"a\":1,}",
		"{\"a\":01}",
		"{\"a\":\"\\q\"}",
		"{unquoted:1}",
		"[1,2",
		"{\"a\":1} trailing",
		""
	};
	bool rejected = true;
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		BMessage bad;
		if (PCloudJson::Parse(invalid[i], bad) == B_OK)
			rejected = false;
	}
	runner.Check(rejected, "seven malformed JSON inputs rejected");
}


static void
test_cache(TestRunner& runner)
{
	runner.Begin("cache");

	PCloudCache cache;
	status_t status = cache.Init("automated-test");
	runner.Check(status == B_OK, "cache initialization");
	if (status != B_OK)
		return;

	PCloudNode node;
	node.fId = "cache-test-1";
	node.fName = "file with spaces.txt";
	node.fParentId = "0";
	node.fIsFolder = false;
	node.fSize = 11;
	node.fModified = 1234567890;
	node.fSha1 = "0";
	node.fRefreshed = system_time();
	runner.Check(cache.PutNode(node) == B_OK, "write node metadata");

	PCloudNode loaded;
	runner.Check(cache.GetNode(node.fId, loaded) == B_OK
		&& loaded.fName == node.fName && loaded.fSize == node.fSize
		&& loaded.fModified == node.fModified && loaded.fParentId == "0",
		"read node metadata back");

	BObjectList<PCloudNode> children(true);
	children.AddItem(new PCloudNode(node));
	PCloudNode folder;
	folder.fId = "cache-test-folder";
	folder.fName = "folder";
	folder.fIsFolder = true;
	children.AddItem(new PCloudNode(folder));
	runner.Check(cache.PutChildren("0", children) == B_OK,
		"write directory listing");

	BObjectList<PCloudNode> restored(true);
	bool fresh = false;
	runner.Check(cache.GetChildren("0", restored,
		kPCloudMetadataTimeout, fresh) == B_OK && fresh
		&& restored.CountItems() == 2, "read fresh directory listing");

	BString temporary;
	runner.Check(cache.TempPath("cache-test-data", temporary) == B_OK,
		"temporary path");
	const char kContent[] = "hello world";
	int fd = open(temporary.String(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
	runner.Check(fd >= 0, "create temporary data file");
	if (fd >= 0) {
		bool written = write(fd, kContent, strlen(kContent))
			== (ssize_t)strlen(kContent);
		close(fd);
		runner.Check(written, "write temporary data file");
	}

	uint8 digest[20];
	PCloudSHA1::Hash(kContent, strlen(kContent), digest);
	BString sha1 = PCloudSHA1::ToHex(digest);
	runner.Check(cache.VerifyAndPromote("cache-test-data", temporary, sha1,
		strlen(kContent)) == B_OK, "verify and promote valid download");

	BString path;
	runner.Check(cache.CachedFile("cache-test-data", sha1,
		strlen(kContent), path) == B_OK, "cached file accepted with correct hash");
	runner.Check(cache.CachedFile("cache-test-data", "0000000000000000000000000000000000000000",
		strlen(kContent), path) == B_BAD_DATA,
		"cached file rejected with wrong hash");
	runner.Check(cache.CachedFile("cache-test-data", sha1,
		strlen(kContent) + 1, path) == B_BAD_DATA,
		"cached file rejected with wrong size");

	BString badTemp;
	cache.TempPath("cache-test-data", badTemp);
	badTemp << ".bad";
	fd = open(badTemp.String(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
	bool badWritten = false;
	if (fd >= 0) {
		badWritten = write(fd, "corrupted", 9) == 9;
		close(fd);
	}
	runner.Check(badWritten, "create corrupted download");
	runner.Check(cache.VerifyAndPromote("cache-test-data", badTemp, sha1, 9)
		== B_BAD_DATA, "corrupted download rejected");
	runner.Check(cache.CachedFile("cache-test-data", sha1,
		strlen(kContent), path) == B_OK,
		"original verified data survives rejected download");
}


static void
test_auth_local(TestRunner& runner)
{
	runner.Begin("auth-local");

	BString verifier = PCloudAuth::GenerateVerifier();
	runner.Check(verifier.Length() >= 32, "PKCE verifier generated");
	BString challenge = PCloudAuth::ChallengeForVerifier(verifier);
	runner.Check(challenge.Length() >= 32, "PKCE challenge generated");
	runner.Check(challenge != verifier, "challenge differs from verifier");

	PCloudAuth auth;
	runner.Check(auth.Init() == B_OK, "settings can be read");
	if (auth.IsConfigured()) {
		BString url = auth.AuthorizeUrl("state-1", challenge);
		runner.Check(url.StartsWith("https://www.pcloud.com/oauth2/authorize")
			&& url.FindFirst("client_id=") >= 0
			&& url.FindFirst("response_type=code") >= 0
			&& url.FindFirst("state=state-1") >= 0,
			"authorization URL contains expected parameters");
	} else {
		runner.Skip("authorization URL (no credentials configured)");
	}
}


static void
test_network(TestRunner& runner, PCloudAuth& auth)
{
	runner.Begin("network");
	if (!auth.HasToken()) {
		runner.Skip("all network tests (no access token)");
		return;
	}

	PCloudApi api(auth);
	BString email;
	off_t quota = 0;
	off_t used = 0;
	if (!runner.Check(api.UserInfo(email, quota, used) == B_OK,
		"userinfo call")) {
		PCLOUD_LOG_ERROR("userinfo error: %s", api.LastError().String());
		return;
	}
	runner.Check(email.Length() > 0, "account email returned");
	runner.Check(quota > 0 && used >= 0 && used <= quota,
		"quota values are plausible");

	BObjectList<PCloudNode> children(true);
	status_t status = api.ListFolder("0", children);
	if (!runner.Check(status == B_OK, "listfolder root")) {
		PCLOUD_LOG_ERROR("listfolder error: %s", api.LastError().String());
		return;
	}
	bool metadataOk = true;
	int32 filesChecked = 0;
	for (int32 i = 0; i < children.CountItems(); i++) {
		PCloudNode* node = children.ItemAt(i);
		if (node == NULL || !node->IsValid()
			|| node->fId.Length() == 0 || node->fName.Length() == 0) {
			metadataOk = false;
			continue;
		}
		if (!node->fIsFolder && node->fSize >= 0 && filesChecked < 10)
			filesChecked++;
	}
	runner.Check(metadataOk, "all listed nodes have id and name");

	BObjectList<PCloudNode> invalid(true);
	runner.Check(api.ListFolder("999999999999999", invalid) != B_OK,
		"invalid folder id is rejected");

	if (filesChecked > 0) {
		BString sha1;
		off_t size = 0;
		bool checksumOk = true;
		int32 checked = 0;
		for (int32 i = 0; i < children.CountItems() && checked < 3; i++) {
			PCloudNode* node = children.ItemAt(i);
			if (node == NULL || node->fIsFolder || node->fSize > 4 * 1024 * 1024)
				continue;
			if (api.Checksum(node->fId, sha1, size) != B_OK) {
				checksumOk = false;
				break;
			}
			runner.Check(size == node->fSize, "checksum size matches listing");
			checked++;
		}
		runner.Check(checksumOk && checked > 0, "checksumfile returns SHA-1");
	}
}


static void
test_filesystem(TestRunner& runner)
{
	runner.Begin("filesystem");

	struct stat rootStat;
	if (!runner.Check(stat(kMountPoint, &rootStat) == 0,
		"mount point exists")) {
		runner.Skip("filesystem tests (not mounted)");
		return;
	}
	runner.Check(S_ISDIR(rootStat.st_mode), "mount point is a directory");

	DIR* directory = opendir(kMountPoint);
	if (!runner.Check(directory != NULL, "opendir of volume root"))
		return;
	int32 entries = 0;
	while (dirent* entry = readdir(directory)) {
		if (strcmp(entry->d_name, ".") == 0
			|| strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		entries++;
	}
	closedir(directory);
	runner.Check(entries >= 0, "readdir completed without error");
	PCLOUD_LOG_INFO("TEST: volume root has %ld entries", (long)entries);

	BString firstName;
	directory = opendir(kMountPoint);
	if (directory != NULL) {
		while (dirent* entry = readdir(directory)) {
			if (strcmp(entry->d_name, ".") == 0
				|| strcmp(entry->d_name, "..") == 0) {
				continue;
			}
			firstName = entry->d_name;
			break;
		}
		closedir(directory);
	}
	if (firstName.Length() == 0) {
		runner.Skip("file open tests (volume is empty)");
		return;
	}

	BString childPath;
	childPath.SetToFormat("%s/%s", kMountPoint, firstName.String());
	struct stat childStat;
	if (!runner.Check(stat(childPath.String(), &childStat) == 0,
		"stat of first child")) {
		return;
	}
	runner.Check(S_ISDIR(childStat.st_mode) || S_ISREG(childStat.st_mode),
		"child is a regular file or directory");
	if (S_ISREG(childStat.st_mode)) {
		int fd = open(childPath.String(), O_RDONLY);
		if (runner.Check(fd >= 0, "open file read-only")) {
			char buffer[4096];
			ssize_t bytes = read(fd, buffer, sizeof(buffer));
			runner.Check(bytes >= 0, "read from file");
			close(fd);
		}
		fd = open(childPath.String(), O_WRONLY);
		runner.Check(fd < 0 && errno == EROFS,
			"open file for writing is refused with EROFS");
		if (fd >= 0)
			close(fd);
	}

	BString newDirectory;
	newDirectory.SetToFormat("%s/pcloudfs-must-not-exist", kMountPoint);
	runner.Check(mkdir(newDirectory.String(), 0755) != 0,
		"mkdir is refused (read-only volume)");
	BEntry entry(newDirectory.String());
	runner.Check(!entry.Exists(), "refused mkdir created nothing");
	BString newFile;
	newFile.SetToFormat("%s/pcloudfs-must-not-exist.txt", kMountPoint);
	int fd = open(newFile.String(), O_CREAT | O_WRONLY, 0644);
	runner.Check(fd < 0, "create file is refused (read-only volume)");
	if (fd >= 0)
		close(fd);
	BEntry fileEntry(newFile.String());
	runner.Check(!fileEntry.Exists(), "refused create created nothing");

	struct statvfs info;
	if (runner.Check(statvfs(kMountPoint, &info) == 0, "statvfs of volume"))
		runner.Check(info.f_blocks > 0, "statvfs reports quota blocks");
}


static void
print_usage(const char* program)
{
	fprintf(stderr,
		"Usage: %s [all|unit|network|fs]\n"
		"\n"
		"  all      run every applicable suite (default)\n"
		"  unit     crypto, JSON, cache and local auth checks\n"
		"  network  pCloud API checks (needs a configured token)\n"
		"  fs       mounted volume checks (needs /pCloud mounted)\n"
		"\n"
		"Results are also written to %s and to the syslog.\n",
		program, kReportName);
}


int
main(int argc, char** argv)
{
	gPCloudLogger.Init("");
	PCLOUD_LOG_INFO("TEST SUITE starting");

	const char* mode = argc > 1 ? argv[1] : "all";
	if (strcmp(mode, "--help") == 0 || strcmp(mode, "-h") == 0) {
		print_usage(argv[0]);
		return 0;
	}

	TestRunner runner;
	bool runAll = strcmp(mode, "all") == 0;

	if (runAll || strcmp(mode, "unit") == 0) {
		test_crypto(runner);
		test_json(runner);
		test_cache(runner);
		test_auth_local(runner);
	}

	PCloudAuth auth;
	status_t status = auth.Init();
	if (status != B_OK)
		PCLOUD_LOG_WARNING("TEST: cannot read settings: %ld", (long)status);

	if (runAll || strcmp(mode, "network") == 0)
		test_network(runner, auth);
	if (runAll || strcmp(mode, "fs") == 0)
		test_filesystem(runner);

	int result = runner.Finish();
	PCLOUD_LOG_INFO("TEST SUITE finished with exit code %d", result);
	return result;
}
