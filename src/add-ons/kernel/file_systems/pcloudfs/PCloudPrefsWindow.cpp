/*
 * Copyright 2026, Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PCloudPrefsWindow.h"

#include "PCloudApi.h"
#include "PCloudAuth.h"
#include "PCloudDefs.h"
#include "PCloudLogger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Alert.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <OS.h>
#include <RadioButton.h>
#include <StringView.h>
#include <TabView.h>
#include <View.h>
#include <FindDirectory.h>
#include <Path.h>
#include <TextControl.h>
#include <Url.h>


struct WorkerArgs {
	BMessenger	messenger;
	uint32		kind;
	BString		clientId;
	BString		clientSecret;
	BString		code;
	BString		path;
};


static bool
is_safe_path(const BString& path)
{
	if (!path.StartsWith("/") || path.Length() < 2)
		return false;
	for (int32 i = 0; i < path.Length(); i++) {
		char c = path[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-'
			|| c == '.') {
			continue;
		}
		return false;
	}
	return true;
}


static status_t
worker_entry(void* data)
{
	WorkerArgs* args = static_cast<WorkerArgs*>(data);
	BString text;
	status_t status = B_OK;

	switch (args->kind) {
		case kPrefsOpenBrowser:
		{
			PCloudAuth auth;
			auth.Init();
			status = auth.Configure(args->clientId, args->clientSecret,
				"http://localhost:8765/callback");
			if (status == B_OK) {
				BString url = auth.AuthorizeUrl("pcloudfs-prefs");
				text = url;
				BUrl browser(url.String());
				browser.OpenWithPreferredApplication();
			}
			break;
		}
		case kPrefsAuthorize:
		{
			PCloudAuth auth;
			auth.Init();
			status = auth.ExchangeCode(args->code);
			if (status == B_OK) {
				text.SetToFormat("Connected as pCloud user %s",
					auth.Uid().String());
			} else {
				text = auth.LastError();
			}
			break;
		}
		case kPrefsCheckStatus:
		{
			PCloudAuth auth;
			auth.Init();
			if (!auth.HasToken()) {
				status = B_PERMISSION_DENIED;
				text = "Not connected yet. Authorize first.";
				break;
			}
			PCloudApi api(auth);
			BString email;
			off_t quota = 0;
			off_t used = 0;
			status = api.UserInfo(email, quota, used);
			if (status == B_OK) {
				text.SetToFormat("%s — %ld of %ld bytes used", email.String(),
					(long)used, (long)quota);
			} else {
				text = api.LastError();
			}
			break;
		}
		case kPrefsMount:
		case kPrefsUnmount:
		case kPrefsShowInTracker:
		{
			if (!is_safe_path(args->path)) {
				status = B_BAD_VALUE;
				text = "The path may contain only letters, digits, '/', "
					"'-', '_' and '.'.";
				break;
			}
			BString command;
			if (args->kind == kPrefsMount) {
				command.SetToFormat("mkdir -p '%s' && mount -t userlandfs "
					"-p pcloudfs '%s'", args->path.String(),
					args->path.String());
			} else if (args->kind == kPrefsUnmount) {
				command.SetToFormat("unmount '%s'", args->path.String());
			} else {
				command.SetToFormat("open '%s'", args->path.String());
			}
			int result = system(command.String());
			if (result != 0) {
				status = B_ERROR;
				text.SetToFormat("The command failed (exit code %d).", result);
			} else {
				text = "Done.";
			}
			break;
		}
		default:
			status = B_BAD_VALUE;
			text = "Unknown action.";
			break;
	}

	BMessage done(kPrefsWorkerDone);
	done.AddInt32("kind", (int32)args->kind);
	done.AddInt32("status", (int32)status);
	done.AddString("text", text.String());
	args->messenger.SendMessage(&done);
	delete args;
	return B_OK;
}


PCloudPrefsWindow::PCloudPrefsWindow()
	:
	BWindow(BRect(80, 80, 620, 520), "pcloudfs", B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_QUIT_ON_WINDOW_CLOSE),
	fTabs(NULL),
	fAccountStatus(NULL),
	fMountStatus(NULL),
	fCachePath(NULL),
	fCacheInfo(NULL),
	fClientId(NULL),
	fClientSecret(NULL),
	fCode(NULL),
	fMountPoint(NULL),
	fOAuthMode(NULL),
	fPasswordMode(NULL),
	fMessenger(this),
	fBusy(false)
{
	SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

	BMenuBar* menuBar = new BMenuBar("menubar");
	BMenu* fileMenu = new BMenu("File");
	fileMenu->AddItem(new BMenuItem("About pcloudfs", new BMessage(B_ABOUT_REQUESTED)));
	fileMenu->AddSeparatorItem();
	fileMenu->AddItem(new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED),
		'Q', B_COMMAND_KEY));
	menuBar->AddItem(fileMenu);

	fTabs = new BTabView("tabs", B_WIDTH_FROM_WIDEST);
	BTab* accountTab = new BTab;
	accountTab->SetLabel("Account");
	fTabs->AddTab(_BuildAccountTab(), accountTab);
	BTab* diskTab = new BTab;
	diskTab->SetLabel("Disk");
	fTabs->AddTab(_BuildMountTab(), diskTab);
	BTab* cacheTab = new BTab;
	cacheTab->SetLabel("Cache");
	fTabs->AddTab(_BuildCacheTab(), cacheTab);

	fAccountStatus = new BStringView("accountStatus", "Not connected.");

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_ITEM_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.Add(menuBar)
		.Add(fTabs)
		.Add(fAccountStatus);
}


PCloudPrefsWindow::~PCloudPrefsWindow()
{
}


BView*
PCloudPrefsWindow::_BuildAccountTab()
{
	BView* view = new BView("account", B_WILL_DRAW);
	view->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

	BStringView* intro = new BStringView("intro",
		"Connect this computer to your pCloud account.");
	fOAuthMode = new BRadioButton("oauth", "OAuth2 (recommended)",
		new BMessage(kPrefsAuthMode));
	fOAuthMode->SetValue(B_CONTROL_ON);
	fOAuthMode->SetTarget(this);
	fPasswordMode = new BRadioButton("password", "E-mail and password",
		new BMessage(kPrefsAuthMode));
	fPasswordMode->SetTarget(this);
	fPasswordMode->SetEnabled(false);

	BStringView* passwordNote = new BStringView("passwordNote",
		"Password login is for accounts without two-factor "
		"authentication and will be added later.");
	fClientId = new BTextControl("clientId", "Client ID:", "", NULL);
	fClientSecret = new BTextControl("clientSecret", "Client secret:", "", NULL);
	fCode = new BTextControl("code", "Code from pCloud:", "", NULL);
	fClientId->SetTarget(this);
	fClientSecret->SetTarget(this);
	fCode->SetTarget(this);

	BButton* openBrowser = new BButton("openBrowser", "Open browser",
		new BMessage(kPrefsOpenBrowser));
	BButton* authorize = new BButton("authorize", "Connect",
		new BMessage(kPrefsAuthorize));
	BButton* check = new BButton("check", "Check status",
		new BMessage(kPrefsCheckStatus));
	openBrowser->SetTarget(this);
	authorize->SetTarget(this);
	check->SetTarget(this);

	BLayoutBuilder::Group<>(view, B_VERTICAL, B_USE_ITEM_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.Add(intro)
		.Add(fOAuthMode)
		.Add(fPasswordMode)
		.Add(passwordNote)
		.AddGrid(B_USE_HALF_ITEM_SPACING, B_USE_HALF_ITEM_SPACING)
			.Add(fClientId->CreateLabelLayoutItem(), 0, 0)
			.Add(fClientId->CreateTextViewLayoutItem(), 1, 0)
			.Add(fClientSecret->CreateLabelLayoutItem(), 0, 1)
			.Add(fClientSecret->CreateTextViewLayoutItem(), 1, 1)
			.Add(fCode->CreateLabelLayoutItem(), 0, 2)
			.Add(fCode->CreateTextViewLayoutItem(), 1, 2)
		.End()
		.AddGroup(B_HORIZONTAL, B_USE_HALF_ITEM_SPACING)
			.Add(openBrowser)
			.Add(authorize)
			.Add(check)
			.AddGlue()
		.End()
		.AddGlue();
	return view;
}


BView*
PCloudPrefsWindow::_BuildMountTab()
{
	BView* view = new BView("mount", B_WILL_DRAW);
	view->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

	BStringView* intro = new BStringView("intro",
		"pCloud appears on your desktop like another disk.");
	fMountPoint = new BTextControl("mountPoint", "Mounted at:", "/pCloud", NULL);
	fMountPoint->SetTarget(this);

	BButton* mount = new BButton("mount", "Mount", new BMessage(kPrefsMount));
	BButton* unmount = new BButton("unmount", "Unmount",
		new BMessage(kPrefsUnmount));
	BButton* show = new BButton("show", "Show in Tracker",
		new BMessage(kPrefsShowInTracker));
	mount->SetTarget(this);
	unmount->SetTarget(this);
	show->SetTarget(this);

	fMountStatus = new BStringView("mountStatus", "Not mounted.");

	BLayoutBuilder::Group<>(view, B_VERTICAL, B_USE_ITEM_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.Add(intro)
		.AddGrid(B_USE_HALF_ITEM_SPACING, B_USE_HALF_ITEM_SPACING)
			.Add(fMountPoint->CreateLabelLayoutItem(), 0, 0)
			.Add(fMountPoint->CreateTextViewLayoutItem(), 1, 0)
		.End()
		.AddGroup(B_HORIZONTAL, B_USE_HALF_ITEM_SPACING)
			.Add(mount)
			.Add(unmount)
			.Add(show)
			.AddGlue()
		.End()
		.Add(fMountStatus)
		.AddGlue();
	return view;
}


BView*
PCloudPrefsWindow::_BuildCacheTab()
{
	BView* view = new BView("cache", B_WILL_DRAW);
	view->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

	BPath path;
	find_directory(B_USER_CACHE_DIRECTORY, &path);
	BString cachePath(path.Path());
	cachePath << "/pcloudfs";

	fCachePath = new BStringView("cachePath", cachePath.String());
	fCacheInfo = new BStringView("cacheInfo",
		"Downloaded files are kept here and verified with SHA-1.");
	BButton* clear = new BButton("clear", "Clear cache" B_UTF8_ELLIPSIS,
		new BMessage(kPrefsClearCache));
	clear->SetTarget(this);
	clear->SetEnabled(false);
	BStringView* note = new BStringView("note",
		"Clearing the cache is disabled in this skeleton: pending "
		"write-ahead records must never be removed automatically.");

	BLayoutBuilder::Group<>(view, B_VERTICAL, B_USE_ITEM_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.Add(new BStringView("heading", "Local cache"))
		.Add(fCachePath)
		.Add(fCacheInfo)
		.Add(clear)
		.Add(note)
		.AddGlue();
	return view;
}


void
PCloudPrefsWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kPrefsAuthMode:
			_UpdateAuthFields();
			break;

		case kPrefsOpenBrowser:
		case kPrefsAuthorize:
		case kPrefsCheckStatus:
		case kPrefsMount:
		case kPrefsUnmount:
		case kPrefsShowInTracker:
			if (fBusy)
				break;
			fBusy = true;
			_SetStatus("Working…");
			_StartWorker(message->what, "");
			break;

		case kPrefsClearCache:
		{
			BAlert* alert = new BAlert("clear",
				"Clearing the cache is not available in this test build. "
				"Your data has not been changed.", "OK");
			alert->Go();
			break;
		}

		case kPrefsWorkerDone:
			fBusy = false;
			_WorkerFinished(message);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
PCloudPrefsWindow::_StartWorker(uint32 kind, const BString& description)
{
	WorkerArgs* args = new WorkerArgs;
	args->messenger = BMessenger(this);
	args->kind = kind;
	args->clientId = fClientId->Text();
	args->clientSecret = fClientSecret->Text();
	args->code = fCode->Text();
	args->path = fMountPoint->Text();

	thread_id thread = spawn_thread(worker_entry, "pcloudfs worker",
		B_LOW_PRIORITY, args);
	if (thread < B_OK) {
		delete args;
		fBusy = false;
		_SetStatus("Could not start the operation.");
		return;
	}
	resume_thread(thread);
}


void
PCloudPrefsWindow::_WorkerFinished(BMessage* message)
{
	int32 status = B_OK;
	BString text;
	message->FindInt32("status", &status);
	message->FindString("text", &text);

	if (status == B_OK) {
		_SetStatus(text);
		_UpdateMountState();
	} else {
		BString title = "pcloudfs";
		if (text.Length() == 0)
			text = "The operation could not be completed.";
		BAlert* alert = new BAlert(title.String(), text.String(), "OK");
		alert->Go();
		_SetStatus(text);
	}
}


void
PCloudPrefsWindow::_SetStatus(const BString& text)
{
	if (fAccountStatus != NULL)
		fAccountStatus->SetText(text.String());
}


void
PCloudPrefsWindow::_UpdateAuthFields()
{
	bool oauth = fOAuthMode->Value() == B_CONTROL_ON;
	fClientId->SetEnabled(oauth);
	fClientSecret->SetEnabled(oauth);
	fCode->SetEnabled(oauth);
}


void
PCloudPrefsWindow::_UpdateMountState()
{
	if (fMountStatus == NULL)
		return;
	BString command;
	command.SetToFormat("test -d '%s'", fMountPoint->Text());
	if (is_safe_path(fMountPoint->Text()) && system(command.String()) == 0)
		fMountStatus->SetText("The disk is available.");
	else
		fMountStatus->SetText("Not mounted.");
}


bool
PCloudPrefsWindow::QuitRequested()
{
	if (fBusy) {
		BAlert* alert = new BAlert("busy",
			"An operation is still running. Wait until it finishes.",
			"OK", NULL, NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->Go();
		return false;
	}
	return BWindow::QuitRequested();
}
