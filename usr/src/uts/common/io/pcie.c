/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)pcie.c	1.10	06/05/14 SMI"

#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/modctl.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/promif.h>		  /* prom_printf */
#include <sys/disp.h>		  /* prom_printf */
#include <sys/pcie.h>
#include <sys/pci_cap.h>
#include <sys/pcie_impl.h>


#ifdef  DEBUG
uint_t pcie_debug_flags = 0;

#define	PCIE_DBG pcie_dbg
static void pcie_dbg(char *fmt, ...);

#else   /* DEBUG */

#define	PCIE_DBG 0 &&

#endif  /* DEBUG */

/* Variable to control default PCI-Express config settings */
ushort_t pcie_command_default = PCI_COMM_SERR_ENABLE |
				PCI_COMM_WAIT_CYC_ENAB |
				PCI_COMM_PARITY_DETECT |
				PCI_COMM_ME |
				PCI_COMM_MAE |
				PCI_COMM_IO;
ushort_t pcie_base_err_default = PCIE_DEVCTL_CE_REPORTING_EN |
				PCIE_DEVCTL_NFE_REPORTING_EN |
				PCIE_DEVCTL_FE_REPORTING_EN |
				PCIE_DEVCTL_UR_REPORTING_EN |
				PCIE_DEVCTL_RO_EN;
uint32_t pcie_aer_uce_mask = 0;
uint32_t pcie_aer_ce_mask = 0;
uint32_t pcie_aer_suce_mask = 0;

/*
 * modload support
 */
extern struct mod_ops mod_miscops;
struct modlmisc modlmisc	= {
	&mod_miscops,	/* Type	of module */
	"PCIE: PCI Express Architecture 1.10"
};

struct modlinkage modlinkage = {
	MODREV_1,
	(void	*)&modlmisc,
	NULL
};

int
_init(void)
{
	int rval;

	rval = mod_install(&modlinkage);
	return (rval);
}

int
_fini()
{
	int		rval;

	rval = mod_remove(&modlinkage);
	return (rval);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

/*
 * PCI-Express child device initialization.
 * This function enables generic pci-express interrupts and error
 * handling.
 *
 * @param pdip		root dip (root nexus's dip)
 * @param cdip		child's dip (device's dip)
 * @return		DDI_SUCCESS or DDI_FAILURE
 */
/* ARGSUSED */
int
pcie_initchild(dev_info_t *cdip)
{
	ddi_acc_handle_t	config_handle;
	uint8_t			header_type;
	uint8_t			bcr;
	uint16_t		command_reg, status_reg;
	uint16_t		cap_ptr;

	if (pci_config_setup(cdip, &config_handle) != DDI_SUCCESS)
		return (DDI_FAILURE);

	/*
	 * Determine the configuration header type.
	 */
	header_type = pci_config_get8(config_handle, PCI_CONF_HEADER);
	PCIE_DBG("%s: header_type=%x\n", ddi_driver_name(cdip), header_type);

	/*
	 * Setup the device's command register
	 */
	status_reg = pci_config_get16(config_handle, PCI_CONF_STAT);
	pci_config_put16(config_handle, PCI_CONF_STAT, status_reg);
	command_reg = pci_config_get16(config_handle, PCI_CONF_COMM);
	command_reg |= pcie_command_default;
	pci_config_put16(config_handle, PCI_CONF_COMM, command_reg);

	PCIE_DBG("%s: command=%x\n", ddi_driver_name(cdip),
	    pci_config_get16(config_handle, PCI_CONF_COMM));

	/*
	 * If the device has a bus control register then program it
	 * based on the settings in the command register.
	 */
	if ((header_type & PCI_HEADER_TYPE_M) == PCI_HEADER_ONE) {
		status_reg = pci_config_get16(config_handle,
		    PCI_BCNF_SEC_STATUS);
		pci_config_put16(config_handle, PCI_BCNF_SEC_STATUS,
		    status_reg);
		bcr = pci_config_get8(config_handle, PCI_BCNF_BCNTRL);
		if (pcie_command_default & PCI_COMM_PARITY_DETECT)
			bcr |= PCI_BCNF_BCNTRL_PARITY_ENABLE;
		if (pcie_command_default & PCI_COMM_SERR_ENABLE)
			bcr |= PCI_BCNF_BCNTRL_SERR_ENABLE;
		bcr |= PCI_BCNF_BCNTRL_MAST_AB_MODE;
		pci_config_put8(config_handle, PCI_BCNF_BCNTRL, bcr);
	}

	if ((PCI_CAP_LOCATE(config_handle, PCI_CAP_ID_PCI_E, &cap_ptr))
		!= DDI_FAILURE)
		pcie_enable_errors(cdip, config_handle);

	pci_config_teardown(&config_handle);

	return (DDI_SUCCESS);
fail:
	cmn_err(CE_WARN, "PCIE init child failed\n");
	return (DDI_FAILURE);
}


/*
 * PCI-Express child device de-initialization.
 * This function disables generic pci-express interrupts and error
 * handling.
 *
 * @param pdip		parent dip (root nexus's dip)
 * @param cdip		child's dip (device's dip)
 * @param arg		pcie private data
 */
/* ARGSUSED */
void
pcie_uninitchild(dev_info_t *cdip)
{
	ddi_acc_handle_t	config_handle;

	if (pci_config_setup(cdip, &config_handle) != DDI_SUCCESS)
		return;

	pcie_disable_errors(cdip, config_handle);

	pci_config_teardown(&config_handle);
}

/* ARGSUSED */
void
pcie_clear_errors(dev_info_t *dip, ddi_acc_handle_t config_handle)
{
	uint16_t		cap_ptr, aer_ptr, dev_type, device_sts;
	int			rval = DDI_FAILURE;

	/* 1. clear the Legacy PCI Errors */
	device_sts = pci_config_get16(config_handle, PCI_CONF_STAT);
	pci_config_put16(config_handle, PCI_CONF_STAT, device_sts);

	if ((PCI_CAP_LOCATE(config_handle, PCI_CAP_ID_PCI_E, &cap_ptr))
			== DDI_FAILURE)
		return;

	rval = PCI_CAP_LOCATE(config_handle, PCI_CAP_XCFG_SPC
		(PCIE_EXT_CAP_ID_AER), &aer_ptr);
	dev_type = PCI_CAP_GET16(config_handle, NULL, cap_ptr,
		PCIE_PCIECAP) & PCIE_PCIECAP_DEV_TYPE_MASK;

	/*
	 * Clear any pending errors
	 */
	/* 2. clear the Advanced PCIe Errors */
	if (rval != DDI_FAILURE) {
		PCI_XCAP_PUT32(config_handle, NULL, aer_ptr, PCIE_AER_CE_STS,
			-1);
		PCI_XCAP_PUT32(config_handle, NULL, aer_ptr, PCIE_AER_UCE_STS,
			-1);

		if (dev_type == PCIE_PCIECAP_DEV_TYPE_PCIE2PCI) {
			PCI_XCAP_PUT32(config_handle, NULL, aer_ptr,
				PCIE_AER_SUCE_STS, -1);
		}
	}

	/* 3. clear the PCIe Errors */
	if ((device_sts = PCI_CAP_GET16(config_handle, NULL, cap_ptr,
		PCIE_DEVSTS)) != PCI_CAP_EINVAL16)
		PCI_CAP_PUT16(config_handle, PCI_CAP_ID_PCI_E, cap_ptr,
			PCIE_DEVSTS, device_sts);

	if (dev_type == PCIE_PCIECAP_DEV_TYPE_PCIE2PCI) {
		device_sts = pci_config_get16(config_handle,
		    PCI_BCNF_SEC_STATUS);
		pci_config_put16(config_handle, PCI_BCNF_SEC_STATUS,
		    device_sts);
	}
}

void
pcie_enable_errors(dev_info_t *dip, ddi_acc_handle_t config_handle)
{
	uint16_t		cap_ptr, aer_ptr, dev_type, device_ctl;
	uint32_t		aer_reg;
	int			rval = DDI_FAILURE;

	/*
	 * Clear any pending errors
	 */
	pcie_clear_errors(dip, config_handle);

	if ((PCI_CAP_LOCATE(config_handle, PCI_CAP_ID_PCI_E, &cap_ptr))
			== DDI_FAILURE)
		return;

	rval = PCI_CAP_LOCATE(config_handle, PCI_CAP_XCFG_SPC
		(PCIE_EXT_CAP_ID_AER), &aer_ptr);
	dev_type = PCI_CAP_GET16(config_handle, NULL, cap_ptr,
		PCIE_PCIECAP) & PCIE_PCIECAP_DEV_TYPE_MASK;

	/*
	 * Enable PCI-Express Baseline Error Handling
	 */
	if ((device_ctl = PCI_CAP_GET16(config_handle, NULL, cap_ptr,
		PCIE_DEVCTL)) != PCI_CAP_EINVAL16) {
		PCI_CAP_PUT16(config_handle, NULL, cap_ptr, PCIE_DEVCTL,
			pcie_base_err_default);

		PCIE_DBG("%s: device control=0x%x->0x%x\n",
			ddi_driver_name(dip), device_ctl, PCI_CAP_GET16
			(config_handle, NULL, cap_ptr, PCIE_DEVCTL));
	}

	/*
	 * Enable PCI-Express Advanced Error Handling if Exists
	 */
	if (rval == DDI_FAILURE) {
		return;
	}

	/* Enable Uncorrectable errors */
	if ((aer_reg = PCI_XCAP_GET32(config_handle, NULL, aer_ptr,
		PCIE_AER_UCE_MASK)) != PCI_CAP_EINVAL32) {
		PCI_XCAP_PUT32(config_handle, NULL, aer_ptr,
			PCIE_AER_UCE_MASK, pcie_aer_uce_mask);
		PCIE_DBG("%s: AER UCE=0x%x->0x%x\n", ddi_driver_name(dip),
			aer_reg, PCI_XCAP_GET32(config_handle, NULL, aer_ptr,
			PCIE_AER_UCE_MASK));
	}

	/* Enable Correctable errors */
	if ((aer_reg = PCI_XCAP_GET32(config_handle, NULL, aer_ptr,
		PCIE_AER_CE_MASK)) != PCI_CAP_EINVAL32) {
		PCI_XCAP_PUT32(config_handle, PCIE_EXT_CAP_ID_AER,
			aer_ptr, PCIE_AER_CE_MASK, pcie_aer_ce_mask);
		PCIE_DBG("%s: AER CE=0x%x->0x%x\n", ddi_driver_name(dip),
			aer_reg, PCI_XCAP_GET32(config_handle, NULL, aer_ptr,
			PCIE_AER_CE_MASK));
	}

	/*
	 * Enable Secondary Uncorrectable errors if this is a bridge
	 */
	if (!(dev_type == PCIE_PCIECAP_DEV_TYPE_PCIE2PCI))
		return;

	/*
	 * Enable secondary bus errors
	 */
	if ((aer_reg = PCI_XCAP_GET32(config_handle, NULL, aer_ptr,
		PCIE_AER_SUCE_MASK)) != PCI_CAP_EINVAL32) {
		PCI_XCAP_PUT32(config_handle, NULL, aer_ptr, PCIE_AER_SUCE_MASK,
			pcie_aer_suce_mask);
		PCIE_DBG("%s: AER SUCE=0x%x->0x%x\n", ddi_driver_name(dip),
			aer_reg, PCI_XCAP_GET32(config_handle,
			PCIE_EXT_CAP_ID_AER, aer_ptr, PCIE_AER_SUCE_MASK));
	}
}

/* ARGSUSED */
void
pcie_disable_errors(dev_info_t *dip, ddi_acc_handle_t config_handle)
{
	uint16_t		cap_ptr, aer_ptr, dev_type;
	int			rval = DDI_FAILURE;

	if ((PCI_CAP_LOCATE(config_handle, PCI_CAP_ID_PCI_E, &cap_ptr))
			== DDI_FAILURE)
		return;

	rval = PCI_CAP_LOCATE(config_handle, PCI_CAP_XCFG_SPC
		(PCIE_EXT_CAP_ID_AER), &aer_ptr);
	dev_type = PCI_CAP_GET16(config_handle, NULL, cap_ptr,
		PCIE_PCIECAP) & PCIE_PCIECAP_DEV_TYPE_MASK;

	/*
	 * Disable PCI-Express Baseline Error Handling
	 */
	PCI_CAP_PUT16(config_handle, NULL, cap_ptr, PCIE_DEVCTL, 0x0);

	/*
	 * Disable PCI-Express Advanced Error Handling if Exists
	 */
	if (rval == DDI_FAILURE) {
		return;
	}

	/* Disable Uncorrectable errors */
	PCI_XCAP_PUT32(config_handle, NULL, aer_ptr, PCIE_AER_UCE_MASK,
		PCIE_AER_UCE_BITS);

	/* Disable Correctable errors */
	PCI_XCAP_PUT32(config_handle, NULL, aer_ptr, PCIE_AER_CE_MASK,
		PCIE_AER_CE_BITS);

	/*
	 * Disable Secondary Uncorrectable errors if this is a bridge
	 */
	if (!(dev_type == PCIE_PCIECAP_DEV_TYPE_PCIE2PCI))
		return;

	/*
	 * Disable secondary bus errors
	 */
	PCI_XCAP_PUT32(config_handle, NULL, aer_ptr, PCIE_AER_SUCE_MASK,
		PCIE_AER_SUCE_BITS);
}

#ifdef	DEBUG
/*
 * This is a temporary stop gap measure.
 * PX runs at PIL 14, which is higher than the clock's PIL.
 * As a results we cannot safely print while servicing interrupts using
 * cmn_err or prom_printf.
 *
 * For debugging purposes set px_dbg_print != 0 to see printf messages
 * during interrupt.
 *
 * When a proper solution is in place this code will disappear.
 * Potential solutions are:
 * o circular buffers
 * o taskq to print at lower pil
 */
int pcie_dbg_print = 0;
static void
pcie_dbg(char *fmt, ...)
{
	va_list ap;

	if (!pcie_debug_flags) {
		return;
	}
	va_start(ap, fmt);
	if (servicing_interrupt()) {
		if (pcie_dbg_print) {
			prom_vprintf(fmt, ap);
		}
	} else {
		prom_vprintf(fmt, ap);
	}
	va_end(ap);
}
#endif	/* DEBUG */
