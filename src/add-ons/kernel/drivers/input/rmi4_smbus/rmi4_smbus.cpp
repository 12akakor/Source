/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Synaptics RMI4 touchpad on SMBus (InterTouch): probe stage.
 *
 * This first stage only finds out how the touchpad behaves on SMBus and
 * writes it to the syslog; it does not drive the pointer yet. The PS/2
 * driver keeps owning the touchpad.
 *
 * By default it does not touch the bus: on a ThinkPad X270 any access to
 * the pad, even an unacknowledged Quick Write, broke its PS/2 side.
 * Only with "probe_rmi true" in the driver settings file
 * (~/config/settings/kernel/drivers/rmi4_smbus) it sends a Quick Write,
 * reads the RMI-on-SMBus protocol version, scans the Page Description
 * Table, reads the F01 device information and counts Host Notify messages
 * for 30 seconds. This breaks the PS/2 touchpad and TrackPoint until the
 * next boot.
 *
 * Sources: Synaptics RMI4 Interfacing Guide 511-000136-01 rev. E
 * (2.3.5 Page Description Table, 3.1 F01 queries, 12.2 RMI on SMBus).
 * Pads with SMBus protocol version 2 or 3 address registers through a
 * mapping table instead of the page select register: a 4 byte entry
 * (16 bit RMI address, byte count, flags) is written with an SMBus block
 * write to command 0x80 + n, after which a block read or write with
 * command n accesses those registers. The guide does not describe this;
 * it is how the Linux rmi4 SMBus transport talks to these pads (no code
 * was taken from it).
 */


#include <ACPI.h>
#include <device_manager.h>
#include <driver_settings.h>
#include <i2c.h>
#include <KernelExport.h>
#include <OS.h>

#include <new>
#include <stdio.h>
#include <string.h>


#define TRACE_ALWAYS(x...)	dprintf("rmi4_smbus: " x)

#define RMI4_SMBUS_DRIVER_NAME	"drivers/input/rmi4_smbus/driver_v1"
// must match I801_SYNAPTICS_RMI4_HID in the i801 SMBus driver
#define RMI4_SMBUS_HID			"SYNA_RMI4_SMBUS"


static const uint8 kVersionCommand = 0xfd;
static const uint8 kMapCommandBase = 0x80;
static const int kMapEntries = 8;
static const uint8 kMapFlagWrite = 0x01;

static const uint16 kPdtStart = 0x00e9;		// first entry on a page
static const uint16 kPdtEnd = 0x0005;
static const int kPdtEntrySize = 6;
static const int kMaxPages = 8;

static const bigtime_t kNotifyWindow = 30000000;


struct map_entry {
	uint16	address;
	uint8	count;
	uint8	flags;
	bool	valid;
};

struct rmi4_function {
	uint8	number;
	uint8	version;
	uint8	interruptCount;
	uint16	queryBase;
	uint16	commandBase;
	uint16	controlBase;
	uint16	dataBase;
};

struct rmi4_probe {
	i2c_device_interface*	i2c;
	i2c_device				device;
	thread_id				thread;
	sem_id					notifySem;
	int32					notifications;
	uint16					lastNotifyData;
	volatile bool			quit;

	map_entry				map[kMapEntries];
	int						nextMapEntry;

	rmi4_function			functions[16];
	int						functionCount;
};


static device_manager_info* sDeviceManager;


//	#pragma mark - SMBus access


static status_t
smbus_quick(rmi4_probe* probe)
{
	return probe->i2c->exec_command(probe->device, I2C_OP_WRITE_STOP, NULL, 0,
		NULL, 0);
}


static status_t
smbus_read_byte(rmi4_probe* probe, uint8 command, uint8* _value)
{
	return probe->i2c->exec_command(probe->device, I2C_OP_READ_STOP,
		&command, 1, _value, 1);
}


static status_t
smbus_block_write(rmi4_probe* probe, uint8 command, const void* data,
	size_t length)
{
	return probe->i2c->exec_command(probe->device, I2C_OP_WRITE_BLOCK,
		&command, 1, (void*)data, length);
}


static status_t
smbus_block_read(rmi4_probe* probe, uint8 command,
	uint8 buffer[I2C_SMBUS_BLOCK_MAX + 1])
{
	return probe->i2c->exec_command(probe->device, I2C_OP_READ_BLOCK,
		&command, 1, buffer, I2C_SMBUS_BLOCK_MAX + 1);
}


//	#pragma mark - RMI register access through the mapping table


static status_t
map_registers(rmi4_probe* probe, uint16 address, uint8 count, bool write,
	uint8* _command)
{
	for (int i = 0; i < kMapEntries; i++) {
		const map_entry& entry = probe->map[i];
		if (!entry.valid || entry.address != address)
			continue;
		if ((write && (entry.flags & kMapFlagWrite) != 0)
			|| (!write && entry.count == count)) {
			*_command = i;
			return B_OK;
		}
	}

	int index = probe->nextMapEntry;
	probe->nextMapEntry = (index + 1) % kMapEntries;

	const uint8 raw[4] = { (uint8)(address & 0xff), (uint8)(address >> 8),
		count, (uint8)(write ? kMapFlagWrite : 0) };
	map_entry& entry = probe->map[index];
	entry.valid = false;

	status_t status = smbus_block_write(probe, kMapCommandBase + index, raw,
		sizeof(raw));
	if (status != B_OK)
		return status;

	entry.address = address;
	entry.count = count;
	entry.flags = raw[3];
	entry.valid = true;
	*_command = index;
	return B_OK;
}


static status_t
rmi_read(rmi4_probe* probe, uint16 address, uint8* data, size_t length)
{
	while (length > 0) {
		uint8 count = length > I2C_SMBUS_BLOCK_MAX
			? I2C_SMBUS_BLOCK_MAX : (uint8)length;
		uint8 command;
		status_t status = map_registers(probe, address, count, false,
			&command);
		if (status != B_OK)
			return status;

		uint8 buffer[I2C_SMBUS_BLOCK_MAX + 1];
		status = smbus_block_read(probe, command, buffer);
		if (status != B_OK)
			return status;
		if (buffer[0] < count)
			return B_BAD_DATA;
		memcpy(data, buffer + 1, count);

		address += count;
		data += count;
		length -= count;
	}
	return B_OK;
}


//	#pragma mark - probe


static void
dump_bytes(const char* what, const uint8* data, size_t length)
{
	char line[3 * 32 + 1];
	size_t used = 0;
	for (size_t i = 0; i < length && i < 32; i++)
		used += snprintf(line + used, sizeof(line) - used, " %02x", data[i]);
	line[used] = '\0';
	TRACE_ALWAYS("%s:%s\n", what, line);
}


static void
scan_pdt(rmi4_probe* probe)
{
	int emptyPages = 0;
	for (int page = 0; page < kMaxPages && emptyPages < 2; page++) {
		uint16 pageStart = page << 8;
		bool empty = true;
		// int, not uint16: the loop counts down past zero on a full page
		for (int offset = kPdtStart; offset >= kPdtEnd;
				offset -= kPdtEntrySize) {
			uint8 raw[kPdtEntrySize];
			status_t status = rmi_read(probe, pageStart + offset, raw,
				sizeof(raw));
			if (status != B_OK) {
				TRACE_ALWAYS("PDT read at 0x%04x failed: %s\n",
					pageStart + offset, strerror(status));
				return;
			}
			// ascending: query, command, control, data base, then
			// version (bits 6:5) and interrupt count (2:0), function
			if (raw[5] == 0x00 || raw[5] == 0xff)
				break;
			empty = false;

			rmi4_function function;
			function.number = raw[5];
			function.version = (raw[4] >> 5) & 0x3;
			function.interruptCount = raw[4] & 0x7;
			function.queryBase = pageStart + raw[0];
			function.commandBase = pageStart + raw[1];
			function.controlBase = pageStart + raw[2];
			function.dataBase = pageStart + raw[3];

			TRACE_ALWAYS("F%02x v%u: %u interrupt(s), query 0x%04x command "
				"0x%04x control 0x%04x data 0x%04x\n", function.number,
				function.version, function.interruptCount, function.queryBase,
				function.commandBase, function.controlBase, function.dataBase);

			if (probe->functionCount < (int)B_COUNT_OF(probe->functions))
				probe->functions[probe->functionCount++] = function;
		}
		emptyPages = empty ? emptyPages + 1 : 0;
	}
}


static const rmi4_function*
find_function(rmi4_probe* probe, uint8 number)
{
	for (int i = 0; i < probe->functionCount; i++) {
		if (probe->functions[i].number == number)
			return &probe->functions[i];
	}
	return NULL;
}


static void
host_notify(void* cookie, i2c_addr address, uint16 data)
{
	// interrupt context
	rmi4_probe* probe = (rmi4_probe*)cookie;
	probe->lastNotifyData = data;
	atomic_add(&probe->notifications, 1);
	release_sem_etc(probe->notifySem, 1, B_DO_NOT_RESCHEDULE);
}


static void
watch_host_notify(rmi4_probe* probe, const rmi4_function* f01)
{
	status_t status = probe->i2c->install_host_notify(probe->device,
		&host_notify, probe);
	if (status != B_OK) {
		TRACE_ALWAYS("no Host Notify: %s\n", strerror(status));
		return;
	}

	TRACE_ALWAYS("counting Host Notify messages for %d s, touch the pad\n",
		(int)(kNotifyWindow / 1000000));

	bigtime_t end = system_time() + kNotifyWindow;
	int logged = 0;
	while (!probe->quit && system_time() < end) {
		status = acquire_sem_etc(probe->notifySem, 1, B_ABSOLUTE_TIMEOUT,
			end);
		if (status != B_OK)
			continue;

		// The interrupt status register (F01 data 1) clears on read and
		// lets the pad send the next notification.
		uint8 interruptStatus = 0;
		status = f01 != NULL
			? rmi_read(probe, f01->dataBase + 1, &interruptStatus, 1)
			: B_ENTRY_NOT_FOUND;
		if (logged < 10) {
			logged++;
			TRACE_ALWAYS("Host Notify 0x%04x, interrupt status 0x%02x (%s)\n",
				probe->lastNotifyData, interruptStatus, strerror(status));
		}
	}

	probe->i2c->uninstall_host_notify(probe->device);
	TRACE_ALWAYS("%" B_PRId32 " Host Notify message(s) in the window\n",
		atomic_get(&probe->notifications));
}


static status_t
probe_thread(void* data)
{
	rmi4_probe* probe = (rmi4_probe*)data;

	bool probeRmi = false;
	void* settings = load_driver_settings("rmi4_smbus");
	if (settings != NULL) {
		probeRmi = get_driver_boolean_parameter(settings, "probe_rmi", false,
			true);
		unload_driver_settings(settings);
	}
	if (!probeRmi) {
		// Any SMBus access to the pad, even a Quick Write it does not
		// acknowledge, disturbs its PS/2 side on a ThinkPad X270: the
		// TrackPoint pass-through stops and the PS/2 driver loses the pad.
		// Stay off the bus until the PS/2 driver has handed the pad over.
		TRACE_ALWAYS("no SMBus access to the pad (probe_rmi in the "
			"rmi4_smbus driver settings turns the probe on)\n");
		return B_OK;
	}

	status_t status = smbus_quick(probe);
	TRACE_ALWAYS("quick write to 0x2c: %s\n", status == B_OK
		? "acknowledged, the pad listens on SMBus" : strerror(status));

	uint8 version;
	status = smbus_read_byte(probe, kVersionCommand, &version);
	if (status != B_OK) {
		TRACE_ALWAYS("reading the SMBus protocol version failed: %s\n",
			strerror(status));
		return status;
	}
	// the register holds the version minus one
	TRACE_ALWAYS("RMI-on-SMBus protocol version %u\n", version + 1);
	if (version + 1 != 2 && version + 1 != 3) {
		TRACE_ALWAYS("unknown protocol version, stopping\n");
		return B_NOT_SUPPORTED;
	}

	scan_pdt(probe);

	const rmi4_function* f01 = find_function(probe, 0x01);
	if (f01 != NULL) {
		uint8 query[21];
		status = rmi_read(probe, f01->queryBase, query, sizeof(query));
		if (status == B_OK) {
			char productID[11];
			for (int i = 0; i < 10; i++)
				productID[i] = query[11 + i] & 0x7f;
			productID[10] = '\0';
			TRACE_ALWAYS("F01: manufacturer %u, properties 0x%02x, product "
				"info %02x %02x, date %u-%u-%u, product \"%s\"\n", query[0],
				query[1], query[2], query[3], 2000 + (query[4] & 0x1f),
				query[5] & 0x0f, query[6] & 0x1f, productID);
		} else
			TRACE_ALWAYS("F01 query read failed: %s\n", strerror(status));

		uint8 deviceStatus[2];
		if (rmi_read(probe, f01->dataBase, deviceStatus, 2) == B_OK) {
			TRACE_ALWAYS("F01: device status 0x%02x, interrupt status "
				"0x%02x\n", deviceStatus[0], deviceStatus[1]);
		}
	}

	// raw query registers of the functions a pointer driver needs
	static const uint8 kInteresting[] = { 0x03, 0x11, 0x12, 0x30, 0x3a };
	for (size_t i = 0; i < B_COUNT_OF(kInteresting); i++) {
		const rmi4_function* function = find_function(probe, kInteresting[i]);
		if (function == NULL)
			continue;
		uint8 query[32];
		if (rmi_read(probe, function->queryBase, query, sizeof(query))
				== B_OK) {
			char what[32];
			snprintf(what, sizeof(what), "F%02x query", function->number);
			dump_bytes(what, query, sizeof(query));
		}
	}

	watch_host_notify(probe, f01);
	return B_OK;
}


//	#pragma mark - driver module


static float
rmi4_smbus_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "i2c") != 0) {
		return 0.0f;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || strcmp(hid, RMI4_SMBUS_HID) != 0) {
		return 0.0f;
	}
	return 0.6f;
}


static status_t
rmi4_smbus_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Synaptics RMI4 touchpad (SMBus)" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, RMI4_SMBUS_DRIVER_NAME, attrs,
		NULL, NULL);
}


static status_t
rmi4_smbus_init_driver(device_node* node, void** _cookie)
{
	rmi4_probe* probe = new(std::nothrow) rmi4_probe;
	if (probe == NULL)
		return B_NO_MEMORY;
	memset((void*)probe, 0, sizeof(*probe));

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&probe->i2c,
		(void**)&probe->device);
	sDeviceManager->put_node(parent);

	probe->notifySem = create_sem(0, "rmi4 host notify");
	if (probe->notifySem < 0) {
		status_t status = probe->notifySem;
		delete probe;
		return status;
	}

	// Off the boot path: the probe may wait for up to 30 seconds.
	probe->thread = spawn_kernel_thread(&probe_thread, "rmi4 smbus probe",
		B_NORMAL_PRIORITY, probe);
	if (probe->thread >= 0)
		resume_thread(probe->thread);

	*_cookie = probe;
	return B_OK;
}


static void
rmi4_smbus_uninit_driver(void* cookie)
{
	rmi4_probe* probe = (rmi4_probe*)cookie;

	probe->quit = true;
	release_sem(probe->notifySem);
	if (probe->thread >= 0) {
		status_t result;
		wait_for_thread(probe->thread, &result);
	}
	delete_sem(probe->notifySem);
	delete probe;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static driver_module_info sRmi4SmbusDriver = {
	{
		RMI4_SMBUS_DRIVER_NAME,
		0,
		NULL
	},

	rmi4_smbus_supports_device,
	rmi4_smbus_register_device,
	rmi4_smbus_init_driver,
	rmi4_smbus_uninit_driver,
	NULL,	// register child devices
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sRmi4SmbusDriver,
	NULL
};
