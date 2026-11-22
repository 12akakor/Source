/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_JOURNAL_H
#define _PCLOUD_JOURNAL_H


#include <Message.h>
#include <ObjectList.h>
#include <String.h>


/*! An append-only write-ahead journal for phase 2.

	A record is [16 byte header][flattened BMessage]. The header contains a
	magic, a type, a length and a CRC32C of the payload. Records are appended
	with a single write followed by fsync(), so a crash can only leave a
	torn tail, never a damaged earlier record. Replay stops at the first
	invalid or truncated record and discards it.

	Phase 2 uses this to record the intent of every mutating operation BEFORE
	it touches pCloud. After a crash the journal is replayed and pending
	operations are either finished or rolled back. This class is not yet used
	by the read-only add-on.
*/
class PCloudJournal {
public:
	struct Record {
		uint32		type;
		BMessage	payload;
	};

						PCloudJournal();
						~PCloudJournal();

		status_t		Init(const BString& path);

		status_t		Append(uint32 type, const BMessage& payload);
		status_t		Replay(BObjectList<Record>& records);
		status_t		Clear();

private:
			BString		fPath;
};


#endif	// _PCLOUD_JOURNAL_H
