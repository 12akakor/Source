/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "PCloudNode.h"


static const char kFieldId[] = "id";
static const char kFieldName[] = "name";
static const char kFieldParentId[] = "parentId";
static const char kFieldIsFolder[] = "isFolder";
static const char kFieldSize[] = "size";
static const char kFieldModified[] = "modified";
static const char kFieldSha1[] = "sha1";
static const char kFieldRefreshed[] = "refreshed";


PCloudNode::PCloudNode()
	:
	fId(),
	fName(),
	fParentId(),
	fIsFolder(false),
	fSize(0),
	fModified(0),
	fSha1(),
	fRefreshed(0)
{
}


status_t
PCloudNode::FromMessage(const BMessage& message)
{
	if (message.FindString(kFieldId, &fId) != B_OK)
		return B_BAD_VALUE;
	message.FindString(kFieldName, &fName);
	message.FindString(kFieldParentId, &fParentId);
	message.FindBool(kFieldIsFolder, &fIsFolder);
	message.FindInt64(kFieldSize, &fSize);
	message.FindInt64(kFieldModified, &fModified);
	message.FindString(kFieldSha1, &fSha1);
	message.FindInt64(kFieldRefreshed, &fRefreshed);
	return B_OK;
}


void
PCloudNode::AddToMessage(BMessage& message) const
{
	message.AddString(kFieldId, fId.String());
	message.AddString(kFieldName, fName.String());
	message.AddString(kFieldParentId, fParentId.String());
	message.AddBool(kFieldIsFolder, fIsFolder);
	message.AddInt64(kFieldSize, fSize);
	message.AddInt64(kFieldModified, fModified);
	message.AddString(kFieldSha1, fSha1.String());
	message.AddInt64(kFieldRefreshed, fRefreshed);
}


bool
PCloudNode::IsValid() const
{
	return fId.Length() > 0 && fName.Length() > 0;
}
