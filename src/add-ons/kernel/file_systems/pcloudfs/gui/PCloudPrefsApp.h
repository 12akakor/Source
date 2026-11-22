/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_PREFS_APP_H
#define _PCLOUD_PREFS_APP_H


#include <Application.h>
#include <String.h>


/*! The pcloudfs preferences application.

	One BApplication owns the process; the BWindow owns its own message loop.
	All longer operations run on a worker thread and report back with a
	BMessage, so the interface keeps responding (Haiku HIG: maintain
	responsiveness).
*/
class PCloudPrefsApp : public BApplication {
public:
						PCloudPrefsApp();

	virtual void		ReadyToRun();
	virtual void		AboutRequested();

			void		Notify(const BString& title, const BString& content);
};


#endif	// _PCLOUD_PREFS_APP_H
