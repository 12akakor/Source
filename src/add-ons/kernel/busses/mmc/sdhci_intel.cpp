/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Changes for RenkuOS by Akakor, 2026: only the Bay Trail OCP setting,
 * with a log line of what was found.
 */
/*!	Intel Bay Trail SD host controllers: their OCP timeout. Linux is the
	reference (drivers/mmc/host/sdhci-acpi.c, sdhci_acpi_byt_setting(),
	and sdhci-pci-core.c, byt_ocp_setting()); the behaviour is
	reimplemented here.
*/


#include "sdhci.h"

#include <string.h>

#include <KernelExport.h>
#include <OS.h>
#include <PCI.h>


#define TRACE_ALWAYS(x...)	dprintf("\33[33msdhci:\33[0m " x)


#if defined(__i386__) || defined(__x86_64__)

// IOSF sideband through the message bus registers of the host bridge
// (Linux: arch/x86/platform/intel/iosf_mbi.c)
#define MBI_MCR						0xd0
#define MBI_MDR						0xd4
#define MBI_MCRX					0xd8
#define MBI_ENABLE					0xf0
#define MBI_CR_READ					0x06
#define MBI_CR_WRITE				0x07

#define BYT_IOSF_SCCEP				0x63
#define BYT_IOSF_OCP_NETCTRL0		0x1078
#define BYT_IOSF_OCP_TIMEOUT_BASE	(7u << 8)


static bool
is_baytrail()
{
	// Linux' soc_intel_is_byt(): an Intel family 6 model 0x37 (Silvermont)
	cpuid_info info;
	if (get_cpuid(&info, 0, 0) != B_OK
		|| strncmp(info.eax_0.vendor_id, "GenuineIntel", 12) != 0
		|| get_cpuid(&info, 1, 0) != B_OK)
		return false;

	uint32 model = (info.eax_1.extended_model << 4) | info.eax_1.model;
	return info.eax_1.family == 6 && model == 0x37;
}


static uint32
mbi_command(uint8 opcode, uint8 port, uint32 offset)
{
	return ((uint32)opcode << 24) | ((uint32)port << 16)
		| ((offset & 0xff) << 8) | MBI_ENABLE;
}


/*!	Bay Trail's SD host controllers have been known to hang, and clearing
	the OCP timeout base of the SCC fabric makes that much rarer (Linux
	commit "mmc: sdhci-acpi: Reduce Baytrail eMMC/SD/SDIO hangs"). The
	setting is shared by all SD controllers; doing it more than once is
	harmless. Does nothing on other CPUs.
*/
void
sdhci_intel_byt_ocp_fixup()
{
	if (!is_baytrail())
		return;

	pci_module_info* pci;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return;

	static spinlock sLock = B_SPINLOCK_INITIALIZER;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sLock);

	uint32 high = BYT_IOSF_OCP_NETCTRL0 & 0xffffff00;
	pci->write_pci_config(0, 0, 0, MBI_MCRX, 4, high);
	pci->write_pci_config(0, 0, 0, MBI_MCR, 4,
		mbi_command(MBI_CR_READ, BYT_IOSF_SCCEP, BYT_IOSF_OCP_NETCTRL0));
	uint32 before = pci->read_pci_config(0, 0, 0, MBI_MDR, 4);

	uint32 after = before;
	if ((before & BYT_IOSF_OCP_TIMEOUT_BASE) != 0) {
		after = before & ~BYT_IOSF_OCP_TIMEOUT_BASE;
		pci->write_pci_config(0, 0, 0, MBI_MDR, 4, after);
		pci->write_pci_config(0, 0, 0, MBI_MCRX, 4, high);
		pci->write_pci_config(0, 0, 0, MBI_MCR, 4,
			mbi_command(MBI_CR_WRITE, BYT_IOSF_SCCEP, BYT_IOSF_OCP_NETCTRL0));
	}

	release_spinlock(&sLock);
	restore_interrupts(state);
	put_module(B_PCI_MODULE_NAME);

	static bool sLogged = false;
	if (!sLogged || after != before) {
		sLogged = true;
		TRACE_ALWAYS("Bay Trail OCP NETCTRL0 %#" B_PRIx32 "%s\n", before,
			after != before ? ", timeout base cleared" : "");
	}
}

#else

void
sdhci_intel_byt_ocp_fixup()
{
}

#endif
