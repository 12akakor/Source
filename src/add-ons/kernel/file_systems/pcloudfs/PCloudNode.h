/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_NODE_H
#define _PCLOUD_NODE_H


#include <Message.h>
#include <String.h>


/*! One pCloud file or folder, in the form the filesystem and the cache use.

	The remote id is kept as a decimal string: pCloud ids are 64 bit values
	and BMessage stores them losslessly only as int64, while filenames and
	journal records use the string form.
*/
class PCloudNode {
public:
						PCloudNode();

		status_t		FromMessage(const BMessage& message);
		void			AddToMessage(BMessage& message) const;

		bool			IsValid() const;

public:
			BString		fId;
			BString		fName;
			BString		fParentId;
			bool		fIsFolder;
			off_t		fSize;
			int64		fModified;
			BString		fSha1;
			bigtime_t	fRefreshed;
};


#endif	// _PCLOUD_NODE_H
