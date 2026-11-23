/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_LOGGER_H
#define _PCLOUD_LOGGER_H


#include <Locker.h>
#include <String.h>
#include <stdarg.h>


/*! Logs to the Haiku syslog and to a file in the user's cache directory.

	The X270 test image copies the syslog into SYSLOG.TXT on the EFI partition,
	so messages written here are available to the tester. All methods are
	thread-safe; userlandfs_server calls the add-on from several threads.
*/
class PCloudLogger {
public:
						PCloudLogger();
						~PCloudLogger();

		status_t		Init(const char* accountTag);

		void			Info(const char* format, ...);
		void			Warning(const char* format, ...);
		void			Error(const char* format, ...);
		void			Debug(const char* format, ...);

private:
		void			_Log(int priority, const char* level, const char* format,
							va_list args);

private:
			BLocker		fLock;
			BString		fLogPath;
			bool		fInitialized;
};


extern PCloudLogger gPCloudLogger;

#define PCLOUD_LOG_INFO(...)	gPCloudLogger.Info(__VA_ARGS__)
#define PCLOUD_LOG_WARNING(...)	gPCloudLogger.Warning(__VA_ARGS__)
#define PCLOUD_LOG_ERROR(...)	gPCloudLogger.Error(__VA_ARGS__)
#ifdef DEBUG
#	define PCLOUD_LOG_DEBUG(...)	gPCloudLogger.Debug(__VA_ARGS__)
#else
#	define PCLOUD_LOG_DEBUG(...)	do { } while (false)
#endif


#endif	// _PCLOUD_LOGGER_H
