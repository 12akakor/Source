/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * ThinkPad hotkeys (ACPI device LEN0268, LEN0068 or IBM0068, "HKEY"): the
 * Fn keys that the embedded controller does not send as keyboard scan codes,
 * published as a keyboard device (/dev/input/keyboard/thinkpad_hkey/0) that
 * the input_server keyboard add-on picks up like any other keyboard.
 *
 * The firmware notifies the device with 0x80 and hands out the events with
 * MHKP; MHKA lists the keys it can report, MHKM enables them one by one and
 * MHKC turns reporting on. The event codes and method conventions were
 * checked against Linux's drivers/platform/x86/thinkpad_acpi.c; no code was
 * taken from Linux.
 */


#include <ACPI.h>
#include <usb/USB_hid.h>
#include <usb/USB_hid_page_consumer.h>
#include <usb/USB_hid_page_generic_desktop.h>
#include <usb/USB_hid_page_telephony.h>

#include <keyboard_mouse_driver.h>
#include <kernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define THINKPAD_HKEY_MODULE_NAME			"drivers/power/thinkpad_hkey/driver_v1"
#define THINKPAD_HKEY_KEYBOARD_MODULE_NAME	"drivers/power/thinkpad_hkey/keyboard_v1"

#define THINKPAD_HKEY_KEYBOARD_BASENAME		"input/keyboard/thinkpad_hkey/%d"
#define THINKPAD_HKEY_PATHID_GENERATOR		"thinkpad_hkey/path_id"

#define HKEY_NOTIFY_EVENT					0x80

// event codes read with MHKP
// MHKA bits of the volume keys: the embedded controller of current models
// also sends these as keyboard scan codes, so they are left off.
#define HKEY_VOLUME_KEYS					(7 << 20)

#define KEY_QUEUE_SIZE						32

#define HID_KEY(page, usage)		(((uint32)(page) << 16) | (uint32)(usage))
#define CONSUMER_KEY(usage)			HID_KEY(B_HID_USAGE_PAGE_CONSUMER, usage)
#define DESKTOP_KEY(usage)	HID_KEY(B_HID_USAGE_PAGE_GENERIC_DESKTOP, usage)

// Generic Desktop "Wireless Radio Button" (HID Usage Tables 1.4, 4.13),
// not in <USB_hid_page_generic_desktop.h>
#define HID_UID_GD_WIRELESS_RADIO_BUTTON	0xc6
// HID has no Bluetooth key: use a vendor defined usage, so that Shortcuts
// can still tell it apart
#define BLUETOOTH_KEY				HID_KEY(0xff00, 0x1314)

//#define TRACE_THINKPAD_HKEY
#ifdef TRACE_THINKPAD_HKEY
#	define TRACE(x...) dprintf("thinkpad_hkey: " x)
#else
#	define TRACE(x...)
#endif
#define ERROR(x...) dprintf("thinkpad_hkey: " x)


static device_manager_info* sDeviceManager;
static acpi_module_info* sACPI;
	// test only: logs every ACPI notification


static void
diag_global_notify(acpi_handle handle, uint32 value, void* context)
{
	char path[128];
	acpi_data buffer;
	buffer.pointer = path;
	buffer.length = sizeof(path);
	if (sACPI->ns_handle_to_pathname(handle, &buffer) != B_OK)
		strlcpy(path, "?", sizeof(path));
	dprintf("diag: acpi notify %s %#" B_PRIx32 "\n", path, value);
}

static const char* const kHids[] = { "LEN0268", "LEN0068", "IBM0068" };


/*!	Hotkeys reported as keys, using the key codes that a USB keyboard
	produces for the HID usage closest to the symbol printed on the key, as
	sony_snc does. What the keys do is left to user space (the Shortcuts
	preferences, for example).
*/
static const struct {
	uint16	event;
	uint32	key;
} kHotkeyMap[] = {
	{ 0x1005, DESKTOP_KEY(HID_UID_GD_WIRELESS_RADIO_BUTTON) },
		// airplane mode
	{ 0x1007, DESKTOP_KEY(B_HID_UID_GD_SYSTEM_DISPLAY_TOGGLE) },
		// switch display
	{ 0x1010, CONSUMER_KEY(B_HID_UID_CON_DISPLAY_BRIGHTNESS_INCREMENT) },
	{ 0x1011, CONSUMER_KEY(B_HID_UID_CON_DISPLAY_BRIGHTNESS_DECREMENT) },
	{ 0x101b, HID_KEY(B_HID_USAGE_PAGE_TELEPHONY, B_HID_UID_TEL_PHONE_MUTE) },
		// microphone mute
	{ 0x101d, CONSUMER_KEY(B_HID_UID_CON_AL_CONTROL_PANEL) },
		// settings
	{ 0x101e, CONSUMER_KEY(B_HID_UID_CON_AC_SEARCH) },
	{ 0x101f, CONSUMER_KEY(B_HID_UID_CON_AL_SELECT_TASK_APPLICATION) },
		// show the open windows
	{ 0x1020, CONSUMER_KEY(B_HID_UID_CON_AL_LOCAL_MACHINE_BROWSER) },
		// "my computer"
	{ 0x1311, CONSUMER_KEY(B_HID_UID_CON_AC_BOOKMARKS) },
		// star (F12 on the X270)
	{ 0x1314, BLUETOOTH_KEY },
		// F10 on the X270
	{ 0x1315, CONSUMER_KEY(B_HID_UID_CON_AL_KEYBOARD_LAYOUT) },
		// keyboard settings (F11 on the X270)
};


typedef struct thinkpad_hkey_device {
	device_node*			node;
	acpi_device_module_info* acpi;
	acpi_device				acpi_cookie;
	bool					notify_installed;

	sem_id					lock;
		// protects the key queue; only taken in thread context (the ACPI
		// notify handler runs in a DPC thread)
	sem_id					key_sem;
		// released once per queued key; readers re-check the queue
	raw_key_info			keys[KEY_QUEUE_SIZE];
	uint32					key_head;
	uint32					key_count;
} thinkpad_hkey_device;


typedef struct thinkpad_hkey_cookie {
	thinkpad_hkey_device*	device;
	volatile bool			closed;
} thinkpad_hkey_cookie;


static status_t
hkey_call(thinkpad_hkey_device* device, const char* method, int count,
	uint32 argument0, uint32 argument1, uint32* _result)
{
	acpi_object_type argObjects[2];
	argObjects[0].object_type = ACPI_TYPE_INTEGER;
	argObjects[0].integer.integer = argument0;
	argObjects[1].object_type = ACPI_TYPE_INTEGER;
	argObjects[1].integer.integer = argument1;

	acpi_objects args;
	args.count = count;
	args.pointer = argObjects;

	acpi_object_type result;
	acpi_data buffer;
	buffer.pointer = &result;
	buffer.length = sizeof(result);

	status_t status = device->acpi->evaluate_method(device->acpi_cookie,
		method, count > 0 ? &args : NULL, _result != NULL ? &buffer : NULL);
	if (status != B_OK || _result == NULL)
		return status;
	if (result.object_type != ACPI_TYPE_INTEGER)
		return B_BAD_DATA;

	*_result = (uint32)result.integer.integer;
	return B_OK;
}


static void
hkey_enable(thinkpad_hkey_device* device)
{
	uint32 version = 0;
	hkey_call(device, "MHKV", 0, 0, 0, &version);

	// MHKA takes an argument from interface version 2 on
	uint32 mask;
	if (hkey_call(device, "MHKA", version >> 8 >= 2 ? 1 : 0, 1, 0, &mask)
			!= B_OK) {
		ERROR("no MHKA, hotkeys not available\n");
		return;
	}
	mask &= ~(uint32)HKEY_VOLUME_KEYS;

	for (uint32 bit = 0; bit < 32; bit++) {
		if ((mask & (1u << bit)) != 0)
			hkey_call(device, "MHKM", 2, bit + 1, 1, NULL);
	}
	if (hkey_call(device, "MHKC", 1, 1, 0, NULL) != B_OK) {
		ERROR("could not enable the hotkeys\n");
		return;
	}
	dprintf("thinkpad_hkey: interface version %#" B_PRIx32 ", keys %#"
		B_PRIx32 "\n", version, mask);
}


//	#pragma mark - key queue


static void
hkey_queue_key(thinkpad_hkey_device* device, uint32 key, bool down,
	bigtime_t when)
{
	if (acquire_sem(device->lock) != B_OK)
		return;

	if (device->key_count == KEY_QUEUE_SIZE) {
		// drop the oldest key
		device->key_head = (device->key_head + 1) % KEY_QUEUE_SIZE;
		device->key_count--;
	}

	raw_key_info& info
		= device->keys[(device->key_head + device->key_count) % KEY_QUEUE_SIZE];
	info.timestamp = when;
	info.keycode = key;
	info.is_keydown = down;
	device->key_count++;

	release_sem_etc(device->lock, 1, B_DO_NOT_RESCHEDULE);
	release_sem_etc(device->key_sem, 1, B_DO_NOT_RESCHEDULE);
}


static void
hkey_handle_event(thinkpad_hkey_device* device, uint32 event)
{
	for (size_t i = 0; i < B_COUNT_OF(kHotkeyMap); i++) {
		if (kHotkeyMap[i].event == event) {
			// the firmware reports a key once; send a press and a release
			bigtime_t when = system_time();
			dprintf("diag: hkey key %#" B_PRIx32 "\n", kHotkeyMap[i].key);
			hkey_queue_key(device, kHotkeyMap[i].key, true, when);
			hkey_queue_key(device, kHotkeyMap[i].key, false, when);
			return;
		}
	}

	if ((event & 0xf000) == 0x1000) {
		// an unmapped key: worth a line, so it can be added
		dprintf("thinkpad_hkey: unmapped key event %#" B_PRIx32 "\n", event);
	} else
		TRACE("ignoring event %#" B_PRIx32 "\n", event);
}


static void
hkey_notify_handler(acpi_handle _device, uint32 value, void* context)
{
	thinkpad_hkey_device* device = (thinkpad_hkey_device*)context;

	if (value != HKEY_NOTIFY_EVENT) {
		TRACE("ignoring notification %#" B_PRIx32 "\n", value);
		return;
	}

	// MHKP hands out the queued events one by one, then 0
	for (int i = 0; i < 64; i++) {
		uint32 event;
		if (hkey_call(device, "MHKP", 0, 0, 0, &event) != B_OK || event == 0)
			break;
		dprintf("diag: hkey event %#" B_PRIx32 "\n", event);
		hkey_handle_event(device, event);
	}
}


//	#pragma mark - keyboard device module API


static status_t
thinkpad_hkey_init_device(void* driverCookie, void** cookie)
{
	*cookie = driverCookie;
	return B_OK;
}


static void
thinkpad_hkey_uninit_device(void* cookie)
{
}


static status_t
thinkpad_hkey_open(void* _device, const char* path, int openMode,
	void** _cookie)
{
	thinkpad_hkey_cookie* cookie
		= (thinkpad_hkey_cookie*)calloc(1, sizeof(*cookie));
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->device = (thinkpad_hkey_device*)_device;
	*_cookie = cookie;
	return B_OK;
}


static status_t
thinkpad_hkey_close(void* _cookie)
{
	thinkpad_hkey_cookie* cookie = (thinkpad_hkey_cookie*)_cookie;

	// Wake up a reader blocked in KB_READ; it loops and sees the flag.
	cookie->closed = true;
	release_sem(cookie->device->key_sem);
	return B_OK;
}


static status_t
thinkpad_hkey_free(void* cookie)
{
	free(cookie);
	return B_OK;
}


static status_t
thinkpad_hkey_read_key(thinkpad_hkey_cookie* cookie, void* buffer,
	size_t length)
{
	thinkpad_hkey_device* device = cookie->device;

	if (buffer == NULL || length < sizeof(raw_key_info)
		|| !IS_USER_ADDRESS(buffer)) {
		return B_BAD_VALUE;
	}

	while (true) {
		if (cookie->closed)
			return B_FILE_ERROR;

		status_t status = acquire_sem(device->lock);
		if (status != B_OK)
			return status;

		if (device->key_count > 0) {
			raw_key_info info = device->keys[device->key_head];
			device->key_head = (device->key_head + 1) % KEY_QUEUE_SIZE;
			device->key_count--;
			release_sem(device->lock);

			return user_memcpy(buffer, &info, sizeof(info));
		}
		release_sem(device->lock);

		status = acquire_sem_etc(device->key_sem, 1, B_CAN_INTERRUPT, 0);
		if (status != B_OK)
			return status;
	}
}


static status_t
thinkpad_hkey_read(void* cookie, off_t position, void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
thinkpad_hkey_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
thinkpad_hkey_control(void* cookie, uint32 op, void* arg, size_t length)
{
	switch (op) {
		case KB_READ:
			return thinkpad_hkey_read_key((thinkpad_hkey_cookie*)cookie, arg,
				length);

		case KB_SET_LEDS:
		case KB_SET_KEY_REPEATING:
		case KB_SET_KEY_NONREPEATING:
		case KB_SET_KEY_REPEAT_RATE:
		case KB_SET_KEY_REPEAT_DELAY:
			// no LEDs, and every key is a press and a release
			return B_OK;
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module API


static float
thinkpad_hkey_support(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "acpi") != 0) {
		return 0.0;
	}

	uint32 deviceType;
	if (sDeviceManager->get_attr_uint32(parent, ACPI_DEVICE_TYPE_ITEM,
			&deviceType, false) != B_OK
		|| deviceType != ACPI_TYPE_DEVICE) {
		return 0.0;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK) {
		return 0.0;
	}
	for (size_t i = 0; i < B_COUNT_OF(kHids); i++) {
		if (strcmp(hid, kHids[i]) == 0)
			return 0.6;
	}
	return 0.0;
}


static status_t
thinkpad_hkey_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "ThinkPad hotkeys" }},
		// keep the notify handler installed even when nobody has the device
		// open, so that firmware events are never left unhandled
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, THINKPAD_HKEY_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
thinkpad_hkey_init_driver(device_node* node, void** _driverCookie)
{
	thinkpad_hkey_device* device
		= (thinkpad_hkey_device*)calloc(1, sizeof(thinkpad_hkey_device));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	device->lock = create_sem(1, "thinkpad_hkey lock");
	device->key_sem = create_sem(0, "thinkpad_hkey keys");
	if (device->lock < 0 || device->key_sem < 0) {
		delete_sem(device->lock);
		delete_sem(device->key_sem);
		free(device);
		return B_NO_MORE_SEMS;
	}

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&device->acpi,
		(void**)&device->acpi_cookie);
	sDeviceManager->put_node(parent);

	status_t status = device->acpi->install_notify_handler(
		device->acpi_cookie, ACPI_DEVICE_NOTIFY, hkey_notify_handler, device);
	if (status != B_OK)
		ERROR("could not install notify handler: %s\n", strerror(status));
	else
		device->notify_installed = true;

	hkey_enable(device);

	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&sACPI) == B_OK) {
		status_t diag = sACPI->install_notify_handler(
			(acpi_handle)~(addr_t)0, ACPI_ALL_NOTIFY, diag_global_notify,
			NULL);
		dprintf("diag: global notify handler: %s\n", strerror(diag));
	}

	*_driverCookie = device;
	return B_OK;
}


static void
thinkpad_hkey_uninit_driver(void* driverCookie)
{
	thinkpad_hkey_device* device = (thinkpad_hkey_device*)driverCookie;

	hkey_call(device, "MHKC", 1, 0, 0, NULL);

	if (device->notify_installed) {
		device->acpi->remove_notify_handler(device->acpi_cookie,
			ACPI_DEVICE_NOTIFY, hkey_notify_handler);
	}

	delete_sem(device->key_sem);
	delete_sem(device->lock);
	free(device);
}


static status_t
thinkpad_hkey_register_child_devices(void* driverCookie)
{
	thinkpad_hkey_device* device = (thinkpad_hkey_device*)driverCookie;

	int pathID = sDeviceManager->create_id(THINKPAD_HKEY_PATHID_GENERATOR);
	if (pathID < 0) {
		ERROR("could not create a path ID\n");
		return B_ERROR;
	}

	char name[B_DEV_NAME_LENGTH];
	snprintf(name, sizeof(name), THINKPAD_HKEY_KEYBOARD_BASENAME, pathID);

	return sDeviceManager->publish_device(device->node, name,
		THINKPAD_HKEY_KEYBOARD_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


driver_module_info thinkpad_hkey_driver_module = {
	{
		THINKPAD_HKEY_MODULE_NAME,
		0,
		NULL
	},

	thinkpad_hkey_support,
	thinkpad_hkey_register_device,
	thinkpad_hkey_init_driver,
	thinkpad_hkey_uninit_driver,
	thinkpad_hkey_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


struct device_module_info thinkpad_hkey_keyboard_module = {
	{
		THINKPAD_HKEY_KEYBOARD_MODULE_NAME,
		0,
		NULL
	},

	thinkpad_hkey_init_device,
	thinkpad_hkey_uninit_device,
	NULL,	// removed

	thinkpad_hkey_open,
	thinkpad_hkey_close,
	thinkpad_hkey_free,
	thinkpad_hkey_read,
	thinkpad_hkey_write,
	NULL,	// io
	thinkpad_hkey_control,
	NULL,	// select
	NULL	// deselect
};


module_info* modules[] = {
	(module_info*)&thinkpad_hkey_driver_module,
	(module_info*)&thinkpad_hkey_keyboard_module,
	NULL
};
