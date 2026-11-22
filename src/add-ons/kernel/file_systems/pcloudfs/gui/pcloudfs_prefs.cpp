/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudDefs.h"
#include "PCloudLogger.h"
#include "PCloudPrefsApp.h"


int
main()
{
	gPCloudLogger.Init("");
	PCLOUD_LOG_INFO("pcloudfs preferences starting");

	PCloudPrefsApp app;
	app.Run();

	PCLOUD_LOG_INFO("pcloudfs preferences stopped");
	return 0;
}
