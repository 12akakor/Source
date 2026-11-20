/*
 * Copyright 2018-2021 Intel Corporation. All rights reserved.
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the BSD 3-Clause license.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The register layout, the firmware image format and the boot sequence follow
 * the Linux Sound Open Firmware driver (sound/soc/sof/intel/atom.h, shim.h,
 * byt.c, atom.c and include/uapi/sound/sof/fw.h), which Intel provides under
 * either the GPL 2.0 or the BSD 3-Clause license.
 */
#ifndef INTEL_LPE_SOF_DSP_H
#define INTEL_LPE_SOF_DSP_H


#include <OS.h>


// DSP memory map in the 2 MB LPE BAR (atom.h)
#define LPE_BAR_SIZE				0x200000
#define LPE_IRAM_OFFSET				0x0c0000
#define LPE_IRAM_SIZE				(80 * 1024)
#define LPE_DRAM_OFFSET				0x100000
#define LPE_DRAM_SIZE				(160 * 1024)
#define LPE_SHIM_OFFSET				0x140000
#define LPE_MBOX_OFFSET				0x144000
#define LPE_MBOX_SIZE				0x1000

// shim registers, 64 bit (shim.h)
#define SHIM_CSR					0x00
#define SHIM_IMRX					0x28
#define SHIM_IMRD					0x30
#define SHIM_IPCX					0x38
#define SHIM_IPCD					0x40

#define SHIM_BYT_CSR_RST			(1 << 0)
#define SHIM_BYT_CSR_VECTOR_SEL		(1 << 1)
#define SHIM_BYT_CSR_STALL			(1 << 2)
#define SHIM_BYT_CSR_PWAITMODE		(1 << 3)

#define SHIM_IMRX_DONE				(1 << 0)
#define SHIM_IMRX_BUSY				(1 << 1)

#define SHIM_BYT_IPC_DONE			(1ULL << 62)
#define SHIM_BYT_IPC_BUSY			(1ULL << 63)

// PCI configuration shadow (the second ACPI memory resource): power state
#define LPE_PCI_PMCSR				0x84

// IPC3 (include/sound/sof/header.h, info.h, trace.h)
#define SOF_GLB_TYPE_MASK			(0xfU << 28)
#define SOF_IPC_FW_READY			(0x7U << 28)
#define SOF_IPC_PANIC_MAGIC			0x0dead000
#define SOF_IPC_PANIC_MAGIC_MASK	0x0ffff000


struct lpe_dsp {
	phys_addr_t		physicalBase;
	size_t			length;
	phys_addr_t		pciBase;
	uint32			irq;

	area_id			area;
	uint8*			registers;
	area_id			pciArea;
	uint8*			pci;

	bool			ready;
	uint32			hostBoxOffset;
	uint32			dspBoxOffset;
};


status_t lpe_dsp_init(lpe_dsp* dsp);
void lpe_dsp_uninit(lpe_dsp* dsp);
status_t lpe_dsp_boot(lpe_dsp* dsp, const uint8* image, size_t size);
void lpe_dsp_poll_messages(lpe_dsp* dsp);


#endif	// INTEL_LPE_SOF_DSP_H
