/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The brightness hot keys of ACPI display outputs (ACPI 6.5, appendix B).

	On many laptops (Lenovo's among them) the brightness keys never reach
	the keyboard: the embedded controller raises an SCI, and the firmware's
	query method notifies the built-in panel's output device (the one with
	_BCL and _BCM below the graphics device) with 0x86 (increase) or 0x87
	(decrease). The operating system is to change the brightness itself.

	This driver catches those notifications and turns them into presses of
	the HID Consumer page's Display Brightness Increment/Decrement keys on a
	keyboard device of its own (input/keyboard/acpi_video/<n>). The keyboard
	input_server add-on reads it like any other keyboard, and the Shortcuts
	preferences bind those keys to "screenmode -b +-step", which sets the
	brightness through the graphics driver and shows the on-screen level.
*/


#include <ACPI.h>
#include <keyboard_mouse_driver.h>
#include <usb/USB_hid.h>
#include <usb/USB_hid_page_consumer.h>

#include <kdevice_manager.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define ACPI_VIDEO_MODULE_NAME			"drivers/power/acpi_video/driver_v1"
#define ACPI_VIDEO_DEVICE_MODULE_NAME	"drivers/power/acpi_video/device_v1"

#define ACPI_VIDEO_BASENAME				"input/keyboard/acpi_video/%d"
#define ACPI_VIDEO_PATHID_GENERATOR		"acpi_video/path_id"

// output device notifications (ACPI 6.5, B.7)
#define ACPI_VIDEO_NOTIFY_CYCLE_BRIGHTNESS	0x85
#define ACPI_VIDEO_NOTIFY_INC_BRIGHTNESS	0x86
#define ACPI_VIDEO_NOTIFY_DEC_BRIGHTNESS	0x87
#define ACPI_VIDEO_NOTIFY_ZERO_BRIGHTNESS	0x88
#define ACPI_VIDEO_NOTIFY_DISPLAY_OFF		0x89

#define KEY_BRIGHTNESS_UP	((B_HID_USAGE_PAGE_CONSUMER << 16) \
	| B_HID_UID_CON_DISPLAY_BRIGHTNESS_INCREMENT)
#define KEY_BRIGHTNESS_DOWN	((B_HID_USAGE_PAGE_CONSUMER << 16) \
	| B_HID_UID_CON_DISPLAY_BRIGHTNESS_DECREMENT)

// key events (a press and a release each) waiting for a reader
#define QUEUE_SIZE	32

//#define TRACE_ACPI_VIDEO
#ifdef TRACE_ACPI_VIDEO
#	define TRACE(x...) dprintf("acpi_video: " x)
#else
#	define TRACE(x...)
#endif
#define ERROR(x...) dprintf("acpi_video: " x)


static device_manager_info* sDeviceManager;
static acpi_module_info* sACPI;


struct acpi_video_device {
	device_node*				node;
	acpi_device_module_info*	acpi;
	acpi_device					acpi_cookie;

	spinlock					lock;
	raw_key_info				queue[QUEUE_SIZE];
	uint32						queue_start;
	uint32						queue_count;
	sem_id						queue_sem;
	int32						open_count;
};


struct acpi_video_cookie {
	acpi_video_device*			device;
	bool						closed;
};


static void
queue_key(acpi_video_device* device, uint32 keycode, bool down, bigtime_t when)
{
	InterruptsSpinLocker locker(device->lock);

	if (device->queue_count == QUEUE_SIZE) {
		// nobody reads: the oldest press is of no use any more
		device->queue_start = (device->queue_start + 1) % QUEUE_SIZE;
		device->queue_count--;
	} else
		release_sem_etc(device->queue_sem, 1, B_DO_NOT_RESCHEDULE);

	raw_key_info& info = device->queue[
		(device->queue_start + device->queue_count) % QUEUE_SIZE];
	info.keycode = keycode;
	info.is_keydown = down;
	info.timestamp = when;
	device->queue_count++;
}


static void
acpi_video_notify_handler(acpi_handle handle, uint32 value, void* context)
{
	acpi_video_device* device = (acpi_video_device*)context;
	uint32 keycode;

	switch (value) {
		case ACPI_VIDEO_NOTIFY_INC_BRIGHTNESS:
			keycode = KEY_BRIGHTNESS_UP;
			break;
		case ACPI_VIDEO_NOTIFY_DEC_BRIGHTNESS:
			keycode = KEY_BRIGHTNESS_DOWN;
			break;
		default:
			TRACE("notification %#" B_PRIx32 " ignored\n", value);
			return;
	}

	TRACE("brightness %s\n", keycode == KEY_BRIGHTNESS_UP ? "up" : "down");

	if (atomic_get(&device->open_count) == 0)
		return;

	// the firmware only tells about the press
	bigtime_t now = system_time();
	queue_key(device, keycode, true, now);
	queue_key(device, keycode, false, now);
}


static status_t
read_key(acpi_video_cookie* cookie, raw_key_info* _info)
{
	acpi_video_device* device = cookie->device;

	while (true) {
		status_t status = acquire_sem_etc(device->queue_sem, 1, B_CAN_INTERRUPT,
			0);
		if (status != B_OK)
			return status;

		InterruptsSpinLocker locker(device->lock);
		if (device->queue_count > 0) {
			*_info = device->queue[device->queue_start];
			device->queue_start = (device->queue_start + 1) % QUEUE_SIZE;
			device->queue_count--;
			return B_OK;
		}
		locker.Unlock();

		// woken by a close
		if (cookie->closed)
			return B_FILE_ERROR;
	}
}


//	#pragma mark - device module API


static status_t
acpi_video_init_device(void* driverCookie, void** cookie)
{
	*cookie = driverCookie;
	return B_OK;
}


static void
acpi_video_uninit_device(void* cookie)
{
}


static status_t
acpi_video_open(void* _device, const char* path, int flags, void** _cookie)
{
	acpi_video_device* device = (acpi_video_device*)_device;

	acpi_video_cookie* cookie = new(std::nothrow) acpi_video_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->device = device;
	cookie->closed = false;
	atomic_add(&device->open_count, 1);

	*_cookie = cookie;
	return B_OK;
}


static status_t
acpi_video_close(void* _cookie)
{
	acpi_video_cookie* cookie = (acpi_video_cookie*)_cookie;
	acpi_video_device* device = cookie->device;

	cookie->closed = true;
	atomic_add(&device->open_count, -1);

	// wake up a reader, which sees no key and the close
	release_sem_etc(device->queue_sem, 1, B_RELEASE_ALL);
	return B_OK;
}


static status_t
acpi_video_free(void* _cookie)
{
	delete (acpi_video_cookie*)_cookie;
	return B_OK;
}


static status_t
acpi_video_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
acpi_video_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
acpi_video_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	acpi_video_cookie* cookie = (acpi_video_cookie*)_cookie;

	switch (op) {
		case KB_READ:
		{
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;

			raw_key_info info;
			status_t status = read_key(cookie, &info);
			if (status != B_OK)
				return status;

			return user_memcpy(buffer, &info, sizeof(info));
		}

		case KB_GET_KEYBOARD_ID:
			// not a keyboard of its own: leave the layout to the real one
			return B_NOT_SUPPORTED;

		case KB_SET_LEDS:
		case KB_SET_KEY_REPEATING:
		case KB_SET_KEY_NONREPEATING:
		case KB_SET_KEY_REPEAT_RATE:
		case KB_SET_KEY_REPEAT_DELAY:
		case KB_SET_CONTROL_ALT_DEL_TIMEOUT:
		case KB_CANCEL_CONTROL_ALT_DEL:
		case KB_DELAY_CONTROL_ALT_DEL:
			// nothing that repeats, no lights
			return B_OK;
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module API


static bool
has_method(acpi_handle device, const char* name)
{
	acpi_handle method;
	return sACPI->get_handle(device, name, &method) == B_OK;
}


static float
acpi_video_support(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| strcmp(bus, "acpi") != 0) {
		return 0.0f;
	}

	uint32 type;
	if (sDeviceManager->get_attr_uint32(parent, ACPI_DEVICE_TYPE_ITEM, &type,
			false) != B_OK
		|| type != ACPI_TYPE_DEVICE) {
		return 0.0f;
	}

	// an output device of the graphics device: they have no _HID, but an
	// _ADR, and the panel's has the brightness methods
	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) == B_OK) {
		return 0.0f;
	}

	const char* path;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_PATH_ITEM, &path,
			false) != B_OK) {
		return 0.0f;
	}

	acpi_handle handle;
	if (sACPI->get_handle(NULL, path, &handle) != B_OK
		|| !has_method(handle, "_ADR") || !has_method(handle, "_BCL")
		|| !has_method(handle, "_BCM")) {
		return 0.0f;
	}

	dprintf("acpi_video: panel output %s\n", path);
	return 0.6f;
}


static status_t
acpi_video_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "ACPI Video Brightness Keys" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, ACPI_VIDEO_MODULE_NAME, attrs,
		NULL, NULL);
}


static status_t
acpi_video_init_driver(device_node* node, void** _driverCookie)
{
	acpi_video_device* device = new(std::nothrow) acpi_video_device;
	if (device == NULL)
		return B_NO_MEMORY;

	memset(device, 0, sizeof(*device));
	device->node = node;
	B_INITIALIZE_SPINLOCK(&device->lock);

	device->queue_sem = create_sem(0, "acpi_video keys");
	if (device->queue_sem < 0) {
		status_t status = device->queue_sem;
		delete device;
		return status;
	}

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&device->acpi,
		(void**)&device->acpi_cookie);
	sDeviceManager->put_node(parent);

	status_t status = device->acpi->install_notify_handler(device->acpi_cookie,
		ACPI_DEVICE_NOTIFY, acpi_video_notify_handler, device);
	if (status != B_OK) {
		ERROR("can't install notify handler: %s\n", strerror(status));
		delete_sem(device->queue_sem);
		delete device;
		return status;
	}

	*_driverCookie = device;
	return B_OK;
}


static void
acpi_video_uninit_driver(void* driverCookie)
{
	acpi_video_device* device = (acpi_video_device*)driverCookie;

	device->acpi->remove_notify_handler(device->acpi_cookie,
		ACPI_DEVICE_NOTIFY, acpi_video_notify_handler);
	delete_sem(device->queue_sem);
	delete device;
}


static status_t
acpi_video_register_child_devices(void* driverCookie)
{
	acpi_video_device* device = (acpi_video_device*)driverCookie;

	int id = sDeviceManager->create_id(ACPI_VIDEO_PATHID_GENERATOR);
	if (id < 0)
		return id;

	char name[B_DEV_NAME_LENGTH];
	snprintf(name, sizeof(name), ACPI_VIDEO_BASENAME, id);

	status_t status = sDeviceManager->publish_device(device->node, name,
		ACPI_VIDEO_DEVICE_MODULE_NAME);
	if (status != B_OK)
		sDeviceManager->free_id(ACPI_VIDEO_PATHID_GENERATOR, id);
	return status;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ B_ACPI_MODULE_NAME, (module_info**)&sACPI },
	{}
};


static driver_module_info sACPIVideoDriverModule = {
	{
		ACPI_VIDEO_MODULE_NAME,
		0,
		NULL
	},

	acpi_video_support,
	acpi_video_register_device,
	acpi_video_init_driver,
	acpi_video_uninit_driver,
	acpi_video_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


static struct device_module_info sACPIVideoDeviceModule = {
	{
		ACPI_VIDEO_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	acpi_video_init_device,
	acpi_video_uninit_device,
	NULL,	// removed

	acpi_video_open,
	acpi_video_close,
	acpi_video_free,
	acpi_video_read,
	acpi_video_write,
	NULL,	// io
	acpi_video_control,
	NULL,	// select
	NULL	// deselect
};


module_info* modules[] = {
	(module_info*)&sACPIVideoDriverModule,
	(module_info*)&sACPIVideoDeviceModule,
	NULL
};
