/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "PCloudErrors.h"


BString
PCloudErrors::Message(int32 code)
{
	switch (code) {
		case 1000:	return "Log in required.";
		case 1001:	return "No full path or name/folderid provided.";
		case 1002:	return "No full path or folderid provided.";
		case 1003:	return "Neither csr or publickey is provided. Please create Certificate Signing Request and pass it as 'csr' parameter or send your 'publickey'.";
		case 1004:	return "No fileid or path provided.";
		case 1005:	return "Unknown content-type requested.";
		case 1006:	return "Please provide flags.";
		case 1007:	return "Invalid or closed file descriptor.";
		case 1008:	return "Please provide lock 'type'.";
		case 1009:	return "Please provide 'offset'.";
		case 1010:	return "Please provide 'length'.";
		case 1011:	return "Please provide 'count'.";
		case 1012:	return "Invalid lock type. Please provide type (supported values: 0, 1, 2).";
		case 1013:	return "Date/time format not understood.";
		case 1014:	return "Thumb can not be created from this file type.";
		case 1015:	return "Please provide valid thumb size. Width and height must be divisible either by 4 or 5 and must be between 16 and 2048 (1024 for height).";
		case 1016:	return "No full topath or toname/tofolderid provided.";
		case 1017:	return "Invalid 'folderid' provided.";
		case 1018:	return "Invalid 'fileid' provided.";
		case 1019:	return "Please provide 'sha1' or 'md5' checksum";
		case 1020:	return "Please provide language.";
		case 1021:	return "Language not supported.";
		case 1022:	return "Please provide 'code'.";
		case 1023:	return "Please provide 'mail' to share folder with.";
		case 1024:	return "Please provide 'permissions' for the share.";
		case 1025:	return "Please provide 'sharerequestid' or 'code' to accept a share.";
		case 1026:	return "Please provide 'sharerequestid'.";
		case 1027:	return "Please provide 'shareid'.";
		case 1028:	return "Please provide link 'code'.";
		case 1029:	return "Please provide 'fileid'.";
		case 1030:	return "Please provide 'linkid'.";
		case 1031:	return "Please provide 'oldpassword'.";
		case 1032:	return "Please provide 'newpassword'.";
		case 1033:	return "Please provide 'mail'.";
		case 1034:	return "Please provide 'password'.";
		case 1035:	return "Please provide 'comment'.";
		case 1036:	return "Please provide 'uploadlinkid'.";
		case 1037:	return "Please provide at least one of 'topath', 'tofolderid' or 'toname'.";
		case 1038:	return "Please provide 'fileids'.";
		case 1039:	return "Please provide 'name'.";
		case 1040:	return "Please provide 'url'.";
		case 1041:	return "Please provide 'message'.";
		case 1042:	return "Please provide 'reason'.";
		case 1900:	return "Upload not found";
		case 2000:	return "Log in failed.";
		case 2001:	return "Invalid file/folder name.";
		case 2002:	return "A component of parent directory does not exist.";
		case 2003:	return "Access denied. You do not have permissions to preform this operation.";
		case 2004:	return "File or folder alredy exists.";
		case 2005:	return "Directory does not exist.";
		case 2006:	return "Folder is not empty.";
		case 2007:	return "Cannot delete the root folder.";
		case 2008:	return "User is over quota.";
		case 2009:	return "File not found.";
		case 2010:	return "Invalid path.";
		case 2011:	return "Requested speed limit too low, see minspeed for minimum.";
		case 2012:	return "Invalid 'code' provided.";
		case 2013:	return "Email is already verified.";
		case 2014:	return "Please verify your email address to preform this action.";
		case 2015:	return "Can not share root folder.";
		case 2016:	return "You can only share your own folders.";
		case 2017:	return "User does not accept shares.";
		case 2018:	return "Invalid 'mail' provided.";
		case 2019:	return "Share request already exists.";
		case 2020:	return "You can't share a folder with yourself.";
		case 2021:	return "Non existing share request. It might be already accepted or cancelled by the sending user.";
		case 2022:	return "Wrong user to accept the share.";
		case 2023:	return "You are trying to place shared folder into another shared folder.";
		case 2024:	return "User already has access to this folder or subfolder of this folder.";
		case 2025:	return "Invalid shareid.";
		case 2026:	return "You can only share your own files or folders. Copy the file to a folder you own if you need to share it.";
		case 2027:	return "Invalid or already deleted link.";
		case 2028:	return "There are active shares or sharerequests for this folder.";
		case 2029:	return "Revision with provided 'revisionid' not found.";
		case 2030:	return "New password is the same as the old one.";
		case 2031:	return "Wrong 'oldpassword' provided.";
		case 2032:	return "Password too short. Minimum length is 6 characters.";
		case 2033:	return "Password can not start or end with space.";
		case 2034:	return "Password does not contain enough different characters. The minimum is 4.";
		case 2035:	return "Password can not contain only consecutive characters.";
		case 2036:	return "Verification 'code' expired. Please request password reset again.";
		case 2037:	return "You need to accept Terms of Service and all other agreements to register.";
		case 2038:	return "User with this email is already registered.";
		case 2039:	return "You have to own the folder for upload.";
		case 2040:	return "Given 'uploadlinkid' not found.";
		case 2041:	return "Connection broken.";
		case 2042:	return "Cannot rename the root folder.";
		case 2043:	return "Cannot move a folder to a subfolder of itself.";
		case 2044:	return "Video links can only be generated for videos.";
		case 3000:	return "SSL error occured. Check sslerror for more information.";
		case 3001:	return "Could not create thumb from the given file.";
		case 3002:	return "Connection to iTunes failed.";
		case 3003:	return "iTunes error.";
		case 4000:	return "Too many login tries from this IP address.";
		case 5000:	return "Internal error. Try again later.";
		case 5001:	return "Internal upload error.";
		case 5002:	return "Internal error, no servers available. Try again later.";
		case 5003:	return "Write error. Try reopening the file.";
		case 5004:	return "Read error. Try reopening the file.";
		case 6000:	return "Not modified";
		case 7001:	return "Invalid link 'code'.";
		case 7002:	return "This link is deleted by the owner.";
		case 7003:	return "This link is deleted bacause of copyright complaint.";
		case 7004:	return "This link has expired.";
		case 7005:	return "This link has reached its traffic limit.";
		case 7006:	return "This link has reached maximum downloads.";
		case 7007:	return "This link has reached its space limit.";
		case 7008:	return "This link has reached its file limit.";
		default:
		{
			BString message;
			message.SetToFormat("Unknown pCloud error %ld", (long)code);
			return message;
		}
	}
}
