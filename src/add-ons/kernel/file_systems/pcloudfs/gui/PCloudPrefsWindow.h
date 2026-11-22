/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_PREFS_WINDOW_H
#define _PCLOUD_PREFS_WINDOW_H


#include <Messenger.h>
#include <String.h>
#include <Window.h>


class BRadioButton;
class BStringView;
class BTabView;
class BTextControl;


enum pcloud_prefs_messages {
	kPrefsOpenBrowser		= 'pbrw',
	kPrefsAuthorize			= 'paut',
	kPrefsCheckStatus		= 'psts',
	kPrefsAuthMode			= 'pamd',
	kPrefsMount				= 'pmnt',
	kPrefsUnmount			= 'pumn',
	kPrefsShowInTracker		= 'pshw',
	kPrefsClearCache		= 'pclr',
	kPrefsWorkerDone		= 'pdon'
};


/*! Single-window preferences dialog.

	The window follows the Haiku HIG: standard controls used for their
	intended purpose, a natural top-to-bottom tab order, layouts instead of
	frozen coordinates, system colours, keyboard shortcuts, and feedback for
	every operation. Network and mount operations are performed off the
	window thread.
*/
class PCloudPrefsWindow : public BWindow {
public:
						PCloudPrefsWindow();
	virtual				~PCloudPrefsWindow();

	virtual void		MessageReceived(BMessage* message);
	virtual bool		QuitRequested();

private:
		BView*			_BuildAccountTab();
		BView*			_BuildMountTab();
		BView*			_BuildCacheTab();

		void			_StartWorker(uint32 kind, const BString& description);
		void			_WorkerFinished(BMessage* message);
		void			_SetStatus(const BString& text);
		void			_UpdateAuthFields();
		void			_UpdateMountState();

private:
		BTabView*		fTabs;
		BStringView*	fAccountStatus;
		BStringView*	fMountStatus;
		BStringView*	fCachePath;
		BStringView*	fCacheInfo;
		BTextControl*	fClientId;
		BTextControl*	fClientSecret;
		BTextControl*	fCode;
		BTextControl*	fMountPoint;
		BRadioButton*	fOAuthMode;
		BRadioButton*	fPasswordMode;
		BMessenger		fMessenger;
		bool			fBusy;
};


#endif	// _PCLOUD_PREFS_WINDOW_H
