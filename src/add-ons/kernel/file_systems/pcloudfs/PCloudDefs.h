/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_DEFS_H
#define _PCLOUD_DEFS_H


#include <SupportDefs.h>


// The host and port of the pCloud API. TLS is mandatory; credentials are
// never sent over a plain connection.
static const char kPCloudApiHost[] = "api.pcloud.com";
static const uint16 kPCloudApiPort = 443;

// The add-on name, as passed to `mount -t userlandfs -p pcloudfs`.
static const char kPCloudFSName[] = "pcloudfs";

// Default timeouts. A slow cloud must not hang Tracker forever, but a large
// download needs enough time to finish.
static const bigtime_t kPCloudConnectTimeout = 30000000;	// 30 s
static const bigtime_t kPCloudRequestTimeout = 180000000;	// 180 s

// Metadata entries are considered fresh for this long. After that a getattr
// or readdir refreshes them from pCloud.
static const bigtime_t kPCloudMetadataTimeout = 60000000;	// 60 s

// Chunk size used for downloads and SHA-1 calculation.
static const size_t kPCloudBufferSize = 64 * 1024;

// Maximum accepted JSON response. Larger replies are rejected instead of
// exhausting the userlandfs_server process.
static const size_t kPCloudMaxResponseSize = 64 * 1024 * 1024;

// Paths below /boot/home/config. find_directory() is used to resolve the
// actual locations, these are only the relative components.
static const char kPCloudSettingsPath[] = "config/settings/pcloudfs/settings";
static const char kPCloudCachePath[] = "config/cache/pcloudfs";
static const char kPCloudLogName[] = "pcloudfs.log";

// BMessage field names used by the settings file.
static const char kSettingAuthType[] = "authType";
static const char kSettingUsername[] = "username";
static const char kSettingPassword[] = "password";
static const char kSettingAccessToken[] = "accessToken";
static const char kSettingUid[] = "uid";

static const char kTwoFactorParameter[] = "code";

// State of a cached node. The values are stored in the cache metadata file.
enum pcloud_cache_state {
	PCLOUD_CACHE_CLEAN = 0,	// verified against pCloud, safe to read
	PCLOUD_CACHE_DIRTY = 1,	// local changes not uploaded yet (phase 2)
	PCLOUD_CACHE_UPLOADING = 2
};


#endif	// _PCLOUD_DEFS_H
