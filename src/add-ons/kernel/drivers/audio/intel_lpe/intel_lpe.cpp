/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Audio for Bay Trail tablets with the LPE audio DSP (ACPI 80860F28) and a
	Realtek RT5640 codec (ACPI 10EC5640) on I2C, such as the Teclast X98 Air
	III.

	First stage: the driver attaches to the codec, switches its master clock
	on through the ACPI _PS0 method of the codec (which forces PMC platform
	clock 3 on), checks the codec identity and watches its jack detection
	inputs. Plugging headphones in or out is logged. Sound output follows in
	later stages (DSP firmware, I2S pipeline, multi_audio device).

	The codec registers are those of the Realtek ALC5640 datasheet: 8 bit
	register index, 16 bit big endian data.
*/


#include <ACPI.h>
#include <Drivers.h>
#include <Errors.h>
#include <KernelExport.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <i2c.h>
#include <kernel.h>


#define LPE_DRIVER_NAME		"drivers/audio/intel_lpe/driver_v1"

#define TRACE_ALWAYS(x...)	dprintf("intel_lpe: " x)

// RT5640 (ALC5640) registers
#define RT5640_RESET			0x00	// write resets, bits 2:1 device id
#define RT5640_PWR_ANLG1		0x63
#define RT5640_JD_CTRL1			0xbb
#define RT5640_IRQ_CTRL1		0xbd
#define RT5640_GPIO_STATUS		0xbf
#define RT5640_GPIO_CTRL1		0xc0
#define RT5640_GEN_CTRL1		0xfa
#define RT5640_GEN_CTRL2		0xfb
#define RT5640_VENDOR_ID		0xfe
#define RT5640_VENDOR_ID_VALUE	0x10ec

// MX-BB: jack detection source in bits 15:13
#define RT5640_JD_SRC_MASK		0xe000
#define RT5640_JD_SRC_JD1		(2 << 13)
// MX-BF: pin and internal status
#define RT5640_STA_GPIO1		(1 << 8)
#define RT5640_STA_GPIO2		(1 << 7)
#define RT5640_STA_JD			(1 << 4)
// MX-FB: JD2 as extra jack detection source
#define RT5640_EN_JD2			(1 << 8)
#define RT5640_STA_JD2			(1 << 11)

// PMC platform clock 3, the codec MCLK (\PCLK + 0x0c in the ACPI tables)
#define PMC_PLT_CLK_BASE		0xfed03060
#define PMC_PLT_CLK3_OFFSET		0x0c
#define PMC_PLT_CLK_CTRL_MASK	0x3
#define PMC_PLT_CLK_FREQ_PLL	0x4		// 19.2 MHz, else 25 MHz XTAL

#define JACK_POLL_INTERVAL		250000


struct lpe_device {
	device_node*			node;
	i2c_device_interface*	i2c;
	i2c_device				i2cCookie;
	acpi_handle				handle;

	thread_id				thread;
	volatile bool			stopThread;
};


static device_manager_info* sDeviceManager;


//	#pragma mark - codec access


static status_t
codec_read(lpe_device* device, uint8 reg, uint16* _value)
{
	uint8 data[2];
	status_t status = device->i2c->acquire_bus(device->i2cCookie);
	if (status != B_OK)
		return status;
	status = device->i2c->exec_command(device->i2cCookie, I2C_OP_READ_STOP,
		&reg, 1, data, sizeof(data));
	device->i2c->release_bus(device->i2cCookie);
	if (status == B_OK)
		*_value = (data[0] << 8) | data[1];
	return status;
}


static status_t
codec_write(lpe_device* device, uint8 reg, uint16 value)
{
	uint8 cmd[3] = { reg, (uint8)(value >> 8), (uint8)value };
	status_t status = device->i2c->acquire_bus(device->i2cCookie);
	if (status != B_OK)
		return status;
	status = device->i2c->exec_command(device->i2cCookie, I2C_OP_WRITE_STOP,
		cmd, sizeof(cmd), NULL, 0);
	device->i2c->release_bus(device->i2cCookie);
	return status;
}


static status_t
codec_update(lpe_device* device, uint8 reg, uint16 mask, uint16 value)
{
	uint16 old;
	status_t status = codec_read(device, reg, &old);
	if (status != B_OK)
		return status;
	return codec_write(device, reg, (old & ~mask) | (value & mask));
}


//	#pragma mark - platform


/*!	Logs how PMC platform clock 3 (the codec MCLK) is set up. */
static void
log_mclk()
{
	void* address;
	area_id area = map_physical_memory("intel_lpe pmc clock",
		PMC_PLT_CLK_BASE & ~(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &address);
	if (area < 0) {
		TRACE_ALWAYS("cannot map the PMC clock registers: %s\n",
			strerror(area));
		return;
	}

	uint32 value = *(volatile uint32*)((addr_t)address
		+ (PMC_PLT_CLK_BASE & (B_PAGE_SIZE - 1)) + PMC_PLT_CLK3_OFFSET);
	delete_area(area);

	static const char* const kControl[] = { "SoC controlled", "forced on",
		"forced off", "reserved" };
	TRACE_ALWAYS("MCLK (PMC platform clock 3) %#" B_PRIx32 ": %s, %s\n",
		value, kControl[value & PMC_PLT_CLK_CTRL_MASK],
		(value & PMC_PLT_CLK_FREQ_PLL) != 0 ? "19.2 MHz" : "25 MHz");
}


/*!	Logs whether the LPE DSP is enabled in ACPI mode. */
static void
log_lpe(acpi_module_info* acpi)
{
	acpi_handle lpe;
	if (acpi->get_handle(NULL, "\\_SB.LPEA", &lpe) != B_OK) {
		TRACE_ALWAYS("no LPE audio DSP (\\_SB.LPEA) in ACPI\n");
		return;
	}

	acpi_object_type result;
	acpi_data buffer;
	buffer.pointer = &result;
	buffer.length = sizeof(result);
	if (acpi->evaluate_method(lpe, "_STA", NULL, &buffer) == B_OK
		&& result.object_type == ACPI_TYPE_INTEGER) {
		TRACE_ALWAYS("LPE audio DSP (\\_SB.LPEA) _STA %#" B_PRIx64 "\n",
			(uint64)result.integer.integer);
	} else
		TRACE_ALWAYS("LPE audio DSP: _STA failed\n");
}


//	#pragma mark - jack detection


static status_t
read_jack_state(lpe_device* device, uint16* _status, uint16* _status2)
{
	status_t status = codec_read(device, RT5640_GPIO_STATUS, _status);
	if (status == B_OK)
		status = codec_read(device, RT5640_GEN_CTRL2, _status2);
	return status;
}


static status_t
jack_thread(void* cookie)
{
	lpe_device* device = (lpe_device*)cookie;
	uint16 lastStatus = 0xffff;
	uint16 lastStatus2 = 0xffff;
	int32 errors = 0;

	while (!device->stopThread) {
		uint16 status, status2;
		if (read_jack_state(device, &status, &status2) != B_OK) {
			if (errors++ < 5)
				TRACE_ALWAYS("reading the jack state failed\n");
			snooze(JACK_POLL_INTERVAL * 4);
			continue;
		}

		if (status != lastStatus || status2 != lastStatus2) {
			TRACE_ALWAYS("jack: JD (source JD1) %d, JD2 %d, GPIO1 %d, "
				"GPIO2 %d (status %#04x, general control 2 %#04x)\n",
				(status & RT5640_STA_JD) != 0,
				(status2 & RT5640_STA_JD2) != 0,
				(status & RT5640_STA_GPIO1) != 0,
				(status & RT5640_STA_GPIO2) != 0, status, status2);
			lastStatus = status;
			lastStatus2 = status2;
		}

		snooze(JACK_POLL_INTERVAL);
	}

	return B_OK;
}


//	#pragma mark - start up


static status_t
start_codec(lpe_device* device)
{
	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) == B_OK) {
		// _PS0 forces the MCLK on
		if (device->handle != NULL) {
			status_t status = acpi->evaluate_method(device->handle, "_PS0",
				NULL, NULL);
			if (status != B_OK)
				TRACE_ALWAYS("codec _PS0 failed: %s\n", strerror(status));
		}
		log_lpe(acpi);
		put_module(B_ACPI_MODULE_NAME);
		snooze(10000);
	}
	log_mclk();

	uint16 vendor, reset;
	status_t status = codec_read(device, RT5640_VENDOR_ID, &vendor);
	if (status == B_OK)
		status = codec_read(device, RT5640_RESET, &reset);
	if (status != B_OK) {
		TRACE_ALWAYS("the codec does not answer: %s\n", strerror(status));
		return status;
	}

	TRACE_ALWAYS("codec vendor id %#04x, device id %d\n", vendor,
		(reset >> 1) & 0x3);
	if (vendor != RT5640_VENDOR_ID_VALUE) {
		TRACE_ALWAYS("not an RT5640 (expected vendor id %#04x)\n",
			RT5640_VENDOR_ID_VALUE);
		return B_DEV_NOT_READY;
	}

	// Watch both dedicated jack detection pins: JD1 through the JD source
	// selection, JD2 as the extra source. Which one the board uses is not
	// known yet; the log shows which one follows the headphones.
	uint16 jdCtrl, genCtrl2;
	codec_read(device, RT5640_JD_CTRL1, &jdCtrl);
	codec_read(device, RT5640_GEN_CTRL2, &genCtrl2);
	TRACE_ALWAYS("before: JD control %#04x, general control 2 %#04x\n",
		jdCtrl, genCtrl2);

	// The JD pins share IN1N and IN2N, which have to be single ended for
	// that (datasheet 7.9). The comparators need the references and the
	// bias, and the debounce the I2S input clock.
	codec_update(device, RT5640_GEN_CTRL1, (1 << 9) | (1 << 8) | (1 << 0),
		(1 << 9) | (1 << 8) | (1 << 0));
	codec_update(device, RT5640_PWR_ANLG1,
		(1 << 15) | (1 << 13) | (1 << 11) | (1 << 4) | (1 << 2),
		(1 << 15) | (1 << 13) | (1 << 11) | (1 << 4) | (1 << 2));
	codec_update(device, RT5640_JD_CTRL1, RT5640_JD_SRC_MASK,
		RT5640_JD_SRC_JD1);
	codec_update(device, RT5640_GEN_CTRL2, RT5640_EN_JD2, RT5640_EN_JD2);

	uint16 genCtrl1, power;
	codec_read(device, RT5640_GEN_CTRL1, &genCtrl1);
	codec_read(device, RT5640_PWR_ANLG1, &power);
	codec_read(device, RT5640_JD_CTRL1, &jdCtrl);
	codec_read(device, RT5640_GEN_CTRL2, &genCtrl2);
	TRACE_ALWAYS("after: general control 1 %#04x, analog power 1 %#04x, "
		"JD control %#04x, general control 2 %#04x\n", genCtrl1, power,
		jdCtrl, genCtrl2);

	return B_OK;
}


//	#pragma mark - driver module API


static float
lpe_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "i2c") != 0) {
		return 0.0;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || strcmp(hid, "10EC5640") != 0) {
		return 0.0;
	}

	return 0.6;
}


static status_t
lpe_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Intel LPE audio (RT5640)" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, LPE_DRIVER_NAME, attrs, NULL,
		NULL);
}


static status_t
lpe_init_driver(device_node* node, void** _driverCookie)
{
	lpe_device* device = (lpe_device*)calloc(1, sizeof(lpe_device));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&device->i2c,
		(void**)&device->i2cCookie);
	uint64 handle = 0;
	if (sDeviceManager->get_attr_uint64(parent, ACPI_DEVICE_HANDLE_ITEM,
			&handle, false) == B_OK) {
		device->handle = (acpi_handle)(addr_t)handle;
	}
	sDeviceManager->put_node(parent);

	status_t status = start_codec(device);
	if (status != B_OK) {
		free(device);
		return status;
	}

	device->thread = spawn_kernel_thread(jack_thread, "intel_lpe jack",
		B_LOW_PRIORITY, device);
	if (device->thread < 0) {
		status = device->thread;
		free(device);
		return status;
	}
	resume_thread(device->thread);

	*_driverCookie = device;
	return B_OK;
}


static void
lpe_uninit_driver(void* driverCookie)
{
	lpe_device* device = (lpe_device*)driverCookie;

	device->stopThread = true;
	status_t result;
	wait_for_thread(device->thread, &result);

	free(device);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static driver_module_info sLpeDriverModule = {
	{
		LPE_DRIVER_NAME,
		0,
		NULL
	},

	lpe_supports_device,
	lpe_register_device,
	lpe_init_driver,
	lpe_uninit_driver,
	NULL,	// register child devices
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sLpeDriverModule,
	NULL
};
