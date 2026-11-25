/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_ERRORS_H
#define _PCLOUD_ERRORS_H


#include <String.h>


/*! Human-readable messages for pCloud API result codes.

	The numeric codes are defined by the pCloud HTTP JSON API documentation
	(pcloudcom/pclouddoc, errors.txt). Unknown codes get a generic message.
*/
class PCloudErrors {
public:
		static BString	Message(int32 code);
};


#endif	// _PCLOUD_ERRORS_H
