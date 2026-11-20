/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


//!	Audio for Intel Bay Trail tablets with an RT5640 codec.
// On Bay Trail the audio does not go through HD Audio: the low power engine
// (LPE, ACPI 80860F28), a DSP with its own SSP (I2S) ports and DMA, feeds a
// codec on I2C. This driver attaches to that codec (Realtek RT5640, ACPI
// 10EC5640) and will drive the LPE as well.
//
// So far it brings up the codec (master clock through ACPI, reset, jack
// detection, logging when headphones are plugged in or out), boots Sound
// Open Firmware on the DSP (sof_dsp.cpp), sets up a fixed playback pipeline
// from memory to SSP0 and the codec, and plays a test tone through the
// speakers once the DSP runs. A multi_audio device follows.


#include <ACPI.h>
#include <KernelExport.h>
#include <device_manager.h>
#include <i2c.h>
#include <kernel.h>

#include <FindDirectory.h>

#include <new>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sof_dsp.h"


#define DRIVER_NAME		"intel_lpe"
#define INTEL_LPE_DRIVER_MODULE_NAME "drivers/audio/hmulti/intel_lpe/driver_v1"

#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)


// RT5640 registers (ALC5640 datasheet, chapter 8); 8 bit register numbers,
// 16 bit big endian values.
#define RT5640_RESET				0x00
#define RT5640_IN1_IN2				0x0d
#define RT5640_PWR_DIG1				0x61
#define RT5640_PWR_ANLG1			0x63
#define RT5640_PWR_ANLG2			0x64
#define RT5640_JD_CTRL				0xbb
#define RT5640_IRQ_CTRL1			0xbd
#define RT5640_IRQ_CTRL2			0xbe
#define RT5640_INT_IRQ_ST			0xbf
#define RT5640_GPIO_CTRL1			0xc0
#define RT5640_GEN_CTRL1			0xfa
#define RT5640_GEN_CTRL2			0xfb
#define RT5640_VENDOR_ID			0xfe
#define RT5640_DEVICE_ID			0xff

#define RT5640_VENDOR_REALTEK		0x10ec

// MX-BF: status of the selected jack detect source
#define RT5640_STATUS_JD			(1 << 4)

#define JACK_POLL_INTERVAL			250000

// The firmware is read from the boot volume, which may not be mounted yet
// when the driver starts.
#define FIRMWARE_NAME				"sof-byt.ri"
#define FIRMWARE_WAIT				(120 * 1000000LL)
#define FIRMWARE_MAX_SIZE			(1024 * 1024)


struct intel_lpe_driver {
	device_node*			node;
	acpi_handle				codecHandle;

	i2c_device_interface*	i2c;
	i2c_device				i2cCookie;

	thread_id				pollThread;
	volatile bool			stopPolling;

	lpe_dsp					dsp;
	bool					dspInitialized;
};


static device_manager_info* sDeviceManager;


//	#pragma mark - RT5640


static status_t
rt5640_read(intel_lpe_driver* driver, uint8 reg, uint16* _value)
{
	uint8 data[2];
	status_t status = driver->i2c->acquire_bus(driver->i2cCookie);
	if (status != B_OK)
		return status;
	status = driver->i2c->exec_command(driver->i2cCookie, I2C_OP_READ_STOP,
		&reg, 1, data, sizeof(data));
	driver->i2c->release_bus(driver->i2cCookie);
	if (status == B_OK)
		*_value = (data[0] << 8) | data[1];
	return status;
}


static status_t
rt5640_write(intel_lpe_driver* driver, uint8 reg, uint16 value)
{
	uint8 command[3] = { reg, (uint8)(value >> 8), (uint8)value };
	status_t status = driver->i2c->acquire_bus(driver->i2cCookie);
	if (status != B_OK)
		return status;
	status = driver->i2c->exec_command(driver->i2cCookie, I2C_OP_WRITE_STOP,
		command, sizeof(command), NULL, 0);
	driver->i2c->release_bus(driver->i2cCookie);
	return status;
}


/*!	Switches on the master clock of the codec. Its ACPI device does that in
	_PS0: it forces platform clock 3 of the PMC on, which feeds the MCLK pin.
*/
static void
rt5640_power_on(intel_lpe_driver* driver)
{
	if (driver->codecHandle == NULL)
		return;

	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return;

	status_t status = acpi->evaluate_method(driver->codecHandle, "_PS0", NULL,
		NULL);
	if (status != B_OK)
		TRACE_ALWAYS("_PS0 of the codec failed: %s\n", strerror(status));
	put_module(B_ACPI_MODULE_NAME);
	snooze(10000);
}


static status_t
rt5640_probe(intel_lpe_driver* driver)
{
	rt5640_power_on(driver);

	uint16 vendor, device;
	status_t status = rt5640_read(driver, RT5640_VENDOR_ID, &vendor);
	if (status == B_OK)
		status = rt5640_read(driver, RT5640_DEVICE_ID, &device);
	if (status != B_OK) {
		TRACE_ALWAYS("codec does not answer: %s\n", strerror(status));
		return status;
	}
	if (vendor != RT5640_VENDOR_REALTEK) {
		TRACE_ALWAYS("unexpected codec vendor %#04x\n", vendor);
		return B_DEVICE_NOT_FOUND;
	}
	TRACE_ALWAYS("RT5640 codec, device id %#04x\n", device);

	// any write to MX-00 resets the codec to its defaults
	status = rt5640_write(driver, RT5640_RESET, 0);
	if (status != B_OK)
		return status;
	snooze(10000);

	// Jack detection as the Android driver of the tablet left the codec
	// (register dump taken while playing to the speakers): JD1 (the IN1N
	// pin, IN1 single ended) is the source, and the analog references and
	// biases that the jack detect comparator needs are on.
	// Android also routes the jack state to the IRQ output (MX-BD, MX-C0);
	// that line ends at a GPIO interrupt of the SoC that nothing handles yet,
	// so it stays off and the driver polls MX-BF instead.
	static const struct {
		uint8	reg;
		uint16	value;
	} kJackDetectSetup[] = {
		{ RT5640_PWR_ANLG1,		0xe818 },
		{ RT5640_GEN_CTRL1,		0x3e71 },
		{ RT5640_GEN_CTRL2,		0x4040 },
		{ RT5640_JD_CTRL,		0x4003 },
	};
	for (size_t i = 0; i < B_COUNT_OF(kJackDetectSetup); i++) {
		rt5640_write(driver, kJackDetectSetup[i].reg,
			kJackDetectSetup[i].value);
	}

	static const uint8 kRegisters[] = {
		RT5640_IN1_IN2, RT5640_PWR_DIG1, RT5640_PWR_ANLG1, RT5640_PWR_ANLG2,
		RT5640_JD_CTRL, RT5640_IRQ_CTRL1, RT5640_IRQ_CTRL2, RT5640_INT_IRQ_ST,
		RT5640_GPIO_CTRL1, RT5640_GEN_CTRL1, RT5640_GEN_CTRL2
	};
	for (size_t i = 0; i < B_COUNT_OF(kRegisters); i++) {
		uint16 value = 0;
		rt5640_read(driver, kRegisters[i], &value);
		TRACE_ALWAYS("MX-%02X = %04x\n", kRegisters[i], value);
	}
	return B_OK;
}


static void start_dsp(intel_lpe_driver* driver);
static void play_test_tone(intel_lpe_driver* driver);


static status_t
jack_poll_thread(void* data)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)data;
	uint16 last = 0xffff;
	uint16 lastJD2 = 0xffff;

	start_dsp(driver);

	while (!driver->stopPolling) {
		if (driver->dspInitialized)
			lpe_dsp_poll_messages(&driver->dsp);

		uint16 status, gen2;
		if (rt5640_read(driver, RT5640_INT_IRQ_ST, &status) == B_OK
			&& rt5640_read(driver, RT5640_GEN_CTRL2, &gen2) == B_OK) {
			if (status != last || (gen2 & (1 << 11)) != lastJD2) {
				TRACE_ALWAYS("jack detect: JD1 %s (MX-BF %04x, MX-FB %04x)\n",
					(status & RT5640_STATUS_JD) != 0 ? "high" : "low",
					status, gen2);
				last = status;
				lastJD2 = gen2 & (1 << 11);
			}
		}
		snooze(JACK_POLL_INTERVAL);
	}
	return B_OK;
}


//	#pragma mark - DSP


/*!	Reads the SOF firmware from data/firmware/intel/sof, where Linux keeps it
	as well.
*/
static status_t
read_firmware(uint8** _data, size_t* _size)
{
	static const directory_which kDirectories[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY,
	};

	int fd = -1;
	char path[B_PATH_NAME_LENGTH];
	for (size_t i = 0; i < B_COUNT_OF(kDirectories) && fd < 0; i++) {
		char base[B_PATH_NAME_LENGTH];
		if (find_directory(kDirectories[i], -1, false, base, sizeof(base))
				!= B_OK) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/firmware/intel/sof/%s", base,
			FIRMWARE_NAME);
		fd = open(path, O_RDONLY);
	}
	if (fd < 0)
		return B_ENTRY_NOT_FOUND;

	off_t size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	if (size <= 0 || size > FIRMWARE_MAX_SIZE) {
		close(fd);
		return B_BAD_DATA;
	}

	uint8* data = (uint8*)malloc(size);
	if (data == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}
	ssize_t bytesRead = read(fd, data, size);
	close(fd);
	if (bytesRead != size) {
		free(data);
		return B_IO_ERROR;
	}

	TRACE_ALWAYS("loaded %s (%" B_PRIdOFF " bytes)\n", path, size);
	*_data = data;
	*_size = size;
	return B_OK;
}


static void
start_dsp(intel_lpe_driver* driver)
{
	uint8* image = NULL;
	size_t size = 0;
	status_t status;
	bigtime_t timeout = system_time() + FIRMWARE_WAIT;
	while ((status = read_firmware(&image, &size)) == B_ENTRY_NOT_FOUND
		&& system_time() < timeout && !driver->stopPolling) {
		snooze(1000000);
	}
	if (status != B_OK) {
		TRACE_ALWAYS("no SOF firmware (%s): %s\n", FIRMWARE_NAME,
			strerror(status));
		return;
	}

	status = lpe_dsp_init(&driver->dsp);
	if (status == B_OK) {
		driver->dspInitialized = true;
		status = lpe_dsp_boot(&driver->dsp, image, size);
	}
	free(image);

	if (status != B_OK) {
		TRACE_ALWAYS("DSP did not boot: %s\n", strerror(status));
		return;
	}

	play_test_tone(driver);
}


//	#pragma mark - playback


struct rt5640_register_value {
	uint8	reg;
	uint16	value;
};

// The codec as Android set it up while it played through the speakers
// (register dump of the stock Android on this tablet), in a safe order:
// PLL, then clocks and interfaces, mixers, power, and the speaker last.
// AIF1 is an I2S consumer with 24 bit data; the system clock comes from
// the PLL (MCLK 25 MHz * 59 / 60 = 24.576 MHz = 512 * 48 kHz).
static const rt5640_register_value kSpeakerPlayback[] = {
	{ 0x81, 0x1c82 },	// PLL: N = 57, K = 2
	{ 0x82, 0xd000 },	// PLL: M = 13
	{ 0x64, 0x0200 },	// power: PLL
	{ 0x80, 0x4000 },	// system clock from the PLL, PLL from MCLK
	{ 0x83, 0x9a00 },	// ASRC
	{ 0x84, 0xf800 },
	{ 0x85, 0x0008 },
	{ 0x89, 0x0000 },
	{ 0x8a, 0x0000 },
	{ 0x70, 0x8008 },	// I2S1: consumer, 24 bit, I2S
	{ 0x71, 0x8000 },
	{ 0x72, 0x8000 },
	{ 0x73, 0x0014 },
	{ 0x74, 0x0c00 },
	{ 0x75, 0x1d00 },
	{ 0x02, 0xa7a7 },	// headphone volume, muted
	{ 0x03, 0xc8c8 },
	{ 0x04, 0x8000 },
	{ 0x05, 0x0000 },
	{ 0x0d, 0x0540 },
	{ 0x0e, 0x0500 },
	{ 0x0f, 0x0808 },
	{ 0x19, 0xafaf },	// DAC1 digital volume, 0 dB
	{ 0x1a, 0xaaaa },
	{ 0x1b, 0x0000 },
	{ 0x1c, 0x8080 },
	{ 0x1d, 0x2f2f },
	{ 0x1e, 0x0000 },
	{ 0x27, 0x7060 },
	{ 0x28, 0x7070 },
	{ 0x29, 0x8080 },	// DAC mixer: interface 1 to DAC1
	{ 0x2a, 0x1414 },	// stereo DAC mixer: DAC L1/R1
	{ 0x2b, 0x5454 },
	{ 0x2c, 0xaa00 },
	{ 0x2d, 0x0000 },
	{ 0x2e, 0x0c00 },
	{ 0x2f, 0x0100 },
	{ 0x3b, 0x0000 },
	{ 0x3c, 0x007b },
	{ 0x3d, 0x0000 },
	{ 0x3e, 0x007b },
	{ 0x45, 0xc000 },
	{ 0x46, 0x0036 },	// speaker mixer: DAC L1
	{ 0x47, 0x0036 },	// speaker mixer: DAC R1
	{ 0x48, 0xe800 },
	{ 0x49, 0x2800 },
	{ 0x4a, 0x0004 },
	{ 0x4c, 0xfc00 },
	{ 0x4d, 0x0000 },
	{ 0x4e, 0x0000 },
	{ 0x4f, 0x01fe },
	{ 0x50, 0x0000 },
	{ 0x51, 0x0000 },
	{ 0x52, 0x01fe },
	{ 0x53, 0xf000 },
	{ 0x8b, 0x0600 },
	{ 0x8c, 0x0334 },
	{ 0x8d, 0x3000 },	// class D: ratio 2x
	{ 0x8e, 0x0009 },
	{ 0x8f, 0x3100 },
	{ 0x90, 0x0646 },
	{ 0x91, 0x0f00 },
	{ 0x92, 0x0000 },
	{ 0x93, 0x3410 },
	{ 0xb0, 0x2080 },
	{ 0xb1, 0x0000 },
	{ 0xb2, 0x0000 },
	{ 0xb4, 0x2206 },
	{ 0xb5, 0x1f00 },
	{ 0xb6, 0x0000 },
	{ 0xb7, 0x0000 },
	{ 0xb8, 0x034b },
	{ 0xb9, 0x0066 },
	{ 0xba, 0x000b },
	{ 0xd0, 0x0680 },
	{ 0xd1, 0x1c17 },
	{ 0xd2, 0x8c00 },
	{ 0xd3, 0xaa20 },
	{ 0xd6, 0x0400 },
	{ 0xd7, 0x0000 },
	{ 0xd9, 0x0809 },
	{ 0xda, 0x0000 },
	{ 0x61, 0x9801 },	// power: I2S1, DAC L1/R1
	{ 0x62, 0x0000 },
	{ 0x65, 0x3000 },	// power: speaker mixers
	{ 0x66, 0xc000 },	// power: speaker volumes
	{ 0x01, 0x0808 },	// speaker volume, unmuted
};

static const uint8 kPlaybackRegisters[] = {
	0x01, 0x61, 0x63, 0x64, 0x65, 0x66, 0x70, 0x80, 0x81, 0x82
};

#define RT5640_SPEAKER_VOLUME		0x01
#define RT5640_SPEAKER_MUTED		0x8888

// one period of a 500 Hz sine at 48 kHz, a quarter of the 24 bit range
static const int32 kSine[96] = {
	0, 137160, 273733, 409134, 542783, 674107,
	802545, 927547, 1048576, 1165115, 1276665, 1382748,
	1482910, 1576722, 1663782, 1743718, 1816187, 1880878,
	1937516, 1985856, 2025693, 2056856, 2079210, 2092662,
	2097152, 2092662, 2079210, 2056856, 2025693, 1985856,
	1937516, 1880878, 1816187, 1743718, 1663782, 1576722,
	1482910, 1382748, 1276665, 1165115, 1048576, 927547,
	802545, 674107, 542783, 409134, 273733, 137160,
	0, -137160, -273733, -409134, -542783, -674107,
	-802545, -927547, -1048576, -1165115, -1276665, -1382748,
	-1482910, -1576722, -1663782, -1743718, -1816187, -1880878,
	-1937516, -1985856, -2025693, -2056856, -2079210, -2092662,
	-2097152, -2092662, -2079210, -2056856, -2025693, -1985856,
	-1937516, -1880878, -1816187, -1743718, -1663782, -1576722,
	-1482910, -1382748, -1276665, -1165115, -1048576, -927547,
	-802545, -674107, -542783, -409134, -273733, -137160,
};

#define TONE_PAGES					3
#define TONE_DURATION				3000000


static status_t
setup_speaker_playback(intel_lpe_driver* driver)
{
	for (size_t i = 0; i < B_COUNT_OF(kSpeakerPlayback); i++) {
		status_t status = rt5640_write(driver, kSpeakerPlayback[i].reg,
			kSpeakerPlayback[i].value);
		if (status != B_OK) {
			TRACE_ALWAYS("writing MX-%02X failed: %s\n",
				kSpeakerPlayback[i].reg, strerror(status));
			return status;
		}
		if (kSpeakerPlayback[i].reg == 0x64)
			snooze(1000);
	}

	for (size_t i = 0; i < B_COUNT_OF(kPlaybackRegisters); i++) {
		uint16 value = 0;
		rt5640_read(driver, kPlaybackRegisters[i], &value);
		TRACE_ALWAYS("playback MX-%02X = %04x\n", kPlaybackRegisters[i],
			value);
	}
	return B_OK;
}


/*!	Plays a 500 Hz tone through the speakers for a few seconds, to test the
	path from memory through the DSP and SSP0 to the codec.
*/
static void
play_test_tone(intel_lpe_driver* driver)
{
	uint8* memory;
	area_id area = create_area("intel_lpe tone", (void**)&memory,
		B_ANY_KERNEL_ADDRESS, (TONE_PAGES + 1) * B_PAGE_SIZE,
		B_32_BIT_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (area < 0) {
		TRACE_ALWAYS("no memory for the tone: %s\n", strerror(area));
		return;
	}

	physical_entry entry;
	if (get_memory_map(memory, B_PAGE_SIZE, &entry, 1) != B_OK) {
		delete_area(area);
		return;
	}

	// page 0 holds the page table, pages 1 to 3 the samples
	phys_addr_t pages[TONE_PAGES];
	for (int i = 0; i < TONE_PAGES; i++)
		pages[i] = entry.address + (i + 1) * B_PAGE_SIZE;
	memset(memory, 0, B_PAGE_SIZE);
	lpe_fill_page_table(memory, pages, TONE_PAGES);

	int32* samples = (int32*)(memory + B_PAGE_SIZE);
	uint32 frames = TONE_PAGES * B_PAGE_SIZE / 8;
	for (uint32 i = 0; i < frames; i++) {
		samples[i * 2] = kSine[i % 96];
		samples[i * 2 + 1] = kSine[i % 96];
	}

	lpe_stream_buffer buffer;
	buffer.pageTable = entry.address;
	buffer.pageCount = TONE_PAGES;
	buffer.size = TONE_PAGES * B_PAGE_SIZE;
	buffer.periodBytes = 48 * 8;
	TRACE_ALWAYS("tone buffer: page table %#" B_PRIxPHYSADDR ", %" B_PRIu32
		" frames\n", entry.address, frames);

	status_t status = setup_speaker_playback(driver);
	if (status == B_OK) {
		status = lpe_dsp_start_playback(&driver->dsp, &buffer);
		if (status != B_OK)
			TRACE_ALWAYS("playback did not start: %s\n", strerror(status));
	}
	if (status == B_OK) {
		for (int i = 0; i < 3; i++) {
			snooze(TONE_DURATION / 3);
			sof_ipc_stream_posn position;
			if (lpe_dsp_read_position(&driver->dsp, &position) == B_OK) {
				TRACE_ALWAYS("position: host %" B_PRIu64 ", DAI %" B_PRIu64
					", xrun %" B_PRId32 "\n", position.host_posn,
					position.dai_posn, position.xrun_size);
			}
			lpe_dsp_poll_messages(&driver->dsp);
		}
		status = lpe_dsp_stop_playback(&driver->dsp);
		TRACE_ALWAYS("playback stopped: %s\n", strerror(status));
	}

	rt5640_write(driver, RT5640_SPEAKER_VOLUME, RT5640_SPEAKER_MUTED);
	delete_area(area);
}


//	#pragma mark - driver module API


static float
intel_lpe_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| strcmp(bus, "i2c") != 0) {
		return 0.0;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK
		|| strcmp(hid, "10EC5640") != 0) {
		return 0.0;
	}

	return 0.6;
}


static status_t
intel_lpe_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Intel LPE audio (RT5640)" }},
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, INTEL_LPE_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
intel_lpe_init_driver(device_node* node, void** _driverCookie)
{
	intel_lpe_driver* driver = new(std::nothrow) intel_lpe_driver;
	if (driver == NULL)
		return B_NO_MEMORY;
	memset(driver, 0, sizeof(*driver));
	driver->node = node;
	driver->pollThread = -1;

	device_node* parent = sDeviceManager->get_parent_node(node);
	uint64 handle = 0;
	if (sDeviceManager->get_attr_uint64(parent, ACPI_DEVICE_HANDLE_ITEM,
			&handle, false) == B_OK) {
		driver->codecHandle = (acpi_handle)(addr_t)handle;
	}
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&driver->i2c, (void**)&driver->i2cCookie);
	sDeviceManager->put_node(parent);

	if (status == B_OK)
		status = rt5640_probe(driver);
	if (status != B_OK) {
		TRACE_ALWAYS("probe failed: %s\n", strerror(status));
		delete driver;
		return status;
	}

	driver->pollThread = spawn_kernel_thread(jack_poll_thread,
		"intel_lpe poll", B_LOW_PRIORITY, driver);
	if (driver->pollThread >= 0)
		resume_thread(driver->pollThread);

	*_driverCookie = driver;
	return B_OK;
}


static void
intel_lpe_uninit_driver(void* driverCookie)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)driverCookie;
	if (driver->pollThread >= 0) {
		driver->stopPolling = true;
		status_t result;
		wait_for_thread(driver->pollThread, &result);
	}
	if (driver->dspInitialized)
		lpe_dsp_uninit(&driver->dsp);
	delete driver;
}


driver_module_info intel_lpe_driver_module = {
	{
		INTEL_LPE_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	intel_lpe_supports_device,
	intel_lpe_register_device,
	intel_lpe_init_driver,
	intel_lpe_uninit_driver,
	NULL,	// register_child_devices
	NULL,	// rescan
	NULL,	// removed
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


module_info* modules[] = {
	(module_info*)&intel_lpe_driver_module,
	NULL
};
