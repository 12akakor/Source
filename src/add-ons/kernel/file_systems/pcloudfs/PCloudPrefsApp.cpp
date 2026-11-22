/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudPrefsApp.h"

#include "PCloudPrefsWindow.h"

#include <Alert.h>
#include <Notification.h>


PCloudPrefsApp::PCloudPrefsApp()
	:
	BApplication("application/x-vnd.12akakor-pcloudfs-prefs")
{
}


void
PCloudPrefsApp::ReadyToRun()
{
	PCloudPrefsWindow* window = new PCloudPrefsWindow();
	window->Show();
}


void
PCloudPrefsApp::AboutRequested()
{
	BAlert* alert = new BAlert("about",
		"pcloudfs\n\n"
		"Native pCloud filesystem for RenkuOS.\n"
		"This build is read-only: you can browse and download, "
		"but not change files in the cloud.\n\n"
		"Version 0.1 (test build)", "OK");
	alert->Go();
}


void
PCloudPrefsApp::Notify(const BString& title, const BString& content)
{
	BNotification notification(B_INFORMATION_NOTIFICATION);
	notification.SetTitle(title);
	notification.SetContent(content);
	notification.Send();
}
