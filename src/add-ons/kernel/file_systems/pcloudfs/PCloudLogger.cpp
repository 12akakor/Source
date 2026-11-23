/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "PCloudLogger.h"

#include "PCloudDefs.h"

#include <stdio.h>
#include <syslog.h>

#include <AutoLocker.h>
#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>


PCloudLogger gPCloudLogger;


PCloudLogger::PCloudLogger()
	:
	fLogPath(),
	fInitialized(false)
{
}


PCloudLogger::~PCloudLogger()
{
}


status_t
PCloudLogger::Init(const char* accountTag)
{
	BPath cache;
	status_t status = find_directory(B_USER_CACHE_DIRECTORY, &cache);
	if (status != B_OK)
		return status;

	BDirectory directory;
	status = directory.SetTo(cache.Path());
	if (status == B_ENTRY_NOT_FOUND) {
		status = create_directory(cache.Path(), 0755);
		if (status == B_OK)
			status = directory.SetTo(cache.Path());
	}
	if (status != B_OK)
		return status;

	BPath logPath(cache);
	if (logPath.Append("pcloudfs") != B_OK)
		return B_ERROR;
	if (directory.SetTo(logPath.Path()) == B_ENTRY_NOT_FOUND) {
		status = create_directory(logPath.Path(), 0755);
		if (status != B_OK)
			return status;
	}
	if (logPath.Append(kPCloudLogName) != B_OK)
		return B_ERROR;

	AutoLocker<BLocker> locker(&fLock);
	fLogPath = logPath.Path();
	fInitialized = true;
	return B_OK;
}


void
PCloudLogger::Info(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	_Log(LOG_INFO, "INFO", format, args);
	va_end(args);
}


void
PCloudLogger::Warning(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	_Log(LOG_WARNING, "WARN", format, args);
	va_end(args);
}


void
PCloudLogger::Error(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	_Log(LOG_ERR, "ERROR", format, args);
	va_end(args);
}


void
PCloudLogger::Debug(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	_Log(LOG_DEBUG, "DEBUG", format, args);
	va_end(args);
}


void
PCloudLogger::_Log(int priority, const char* level, const char* format,
	va_list args)
{
	BString message;
	message.SetToFormatVarArgs(format, args);
	if (message.Length() == 0)
		return;

	syslog(LOG_DAEMON | priority, "pcloudfs: %s", message.String());

	if (!fInitialized)
		return;

	AutoLocker<BLocker> locker(&fLock);
	BFile file(fLogPath.String(), B_CREATE_FILE | B_OPEN_AT_END | B_WRITE_ONLY);
	if (file.InitCheck() != B_OK)
		return;
	BString line;
	line.SetToFormat("%s: %s\n", level, message.String());
	file.Write(line.String(), line.Length());
}
