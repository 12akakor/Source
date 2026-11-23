/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#ifndef _PCLOUD_PREFS_WINDOW_H
#define _PCLOUD_PREFS_WINDOW_H


#include <Messenger.h>
#include <String.h>
#include <Window.h>


class BButton;
class BStringView;
class BTabView;
class BTextControl;


enum pcloud_prefs_messages {
	kPrefsLogin			= 'plgn',
	kPrefsCheckStatus	= 'psts',
	kPrefsMount			= 'pmnt',
	kPrefsUnmount		= 'pumn',
	kPrefsShowInTracker	= 'pshw',
	kPrefsClearCache	= 'pclr',
	kPrefsWorkerDone	= 'pdon'
};


/*! Single-window preferences dialog.

	Follows the Haiku HIG: standard controls, natural tab order, layouts
	instead of frozen coordinates, system colours, feedback for every action,
	and all slow work on a worker thread.
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
		void			_UpdateMountState();

private:
		BTabView*		fTabs;
		BStringView*	fAccountStatus;
		BStringView*	fMountStatus;
		BStringView*	fCachePath;
		BStringView*	fCacheInfo;
		BTextControl*	fUsername;
		BTextControl*	fPassword;
		BTextControl*	fMountPoint;
		BButton*		fLoginButton;
		BMessenger		fMessenger;
		bool			fBusy;
};


#endif	// _PCLOUD_PREFS_WINDOW_H
