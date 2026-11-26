/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Turns the volume keys of keyboards and tablet buttons into volume
	changes of the system mixer. The keyboard drivers report these keys with
	the key codes of the USB HID consumer page; no keymap has characters for
	them, so they arrive as B_UNMAPPED_KEY_DOWN.
*/


#include <new>

#include <InputServerFilter.h>
#include <Message.h>
#include <Roster.h>

#include <unistd.h>


#define KEY_MUTE			0x000c00e2
#define KEY_VOLUME_UP		0x000c00e9
#define KEY_VOLUME_DOWN		0x000c00ea


class MediaKeysInputFilter : public BInputServerFilter {
public:
	virtual	filter_result		Filter(BMessage* message, BList* _list);
};


extern "C" BInputServerFilter*
instantiate_input_filter()
{
	return new(std::nothrow) MediaKeysInputFilter();
}


filter_result
MediaKeysInputFilter::Filter(BMessage* message, BList* _list)
{
	if (message->what != B_UNMAPPED_KEY_DOWN)
		return B_DISPATCH_MESSAGE;

	int32 key;
	if (message->FindInt32("key", &key) != B_OK)
		return B_DISPATCH_MESSAGE;

	// setvolume changes the master gain of the system mixer, 3 dB a step
	const char* option;
	switch (key) {
		case KEY_VOLUME_UP:
			option = "--increase";
			break;
		case KEY_VOLUME_DOWN:
			option = "--decrease";
			break;
		case KEY_MUTE:
			option = "--togglemute";
			break;
		default:
			return B_DISPATCH_MESSAGE;
	}

	const char* arguments[] = { "/bin/setvolume", option, NULL };
	thread_id thread = load_image(2, arguments, (const char**)environ);
	if (thread >= B_OK)
		resume_thread(thread);

	return B_SKIP_MESSAGE;
}
