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

#pragma ident	"@(#)pci_tools.c	1.9	06/07/24 SMI"

#include <sys/types.h>
#include <sys/mkdev.h>
#include <sys/stat.h>
#include <sys/sunddi.h>
#include <vm/seg_kmem.h>
#include <sys/machparam.h>
#include <sys/sunndi.h>
#include <sys/ontrap.h>
#include <sys/psm.h>
#include <sys/pcie.h>
#include <sys/hotplug/pci/pcihp.h>
#include <sys/pci_cfgspace.h>
#include <sys/pci_tools.h>
#include <io/pci/pci_tools_ext.h>
#include <io/pcplusmp/apic.h>
#include <io/pci/pci_var.h>
#include <sys/promif.h>
#include <sys/x86_archext.h>
#include <sys/cpuvar.h>

#define	PCIEX_BDF_OFFSET_DELTA	4
#define	PCIEX_REG_FUNC_SHIFT	(PCI_REG_FUNC_SHIFT + PCIEX_BDF_OFFSET_DELTA)
#define	PCIEX_REG_DEV_SHIFT	(PCI_REG_DEV_SHIFT + PCIEX_BDF_OFFSET_DELTA)
#define	PCIEX_REG_BUS_SHIFT	(PCI_REG_BUS_SHIFT + PCIEX_BDF_OFFSET_DELTA)

#define	SUCCESS	0

int pcitool_debug = 0;

/*
 * Offsets of BARS in config space.  First entry of 0 means config space.
 * Entries here correlate to pcitool_bars_t enumerated type.
 */
static uint8_t pci_bars[] = {
	0x0,
	PCI_CONF_BASE0,
	PCI_CONF_BASE1,
	PCI_CONF_BASE2,
	PCI_CONF_BASE3,
	PCI_CONF_BASE4,
	PCI_CONF_BASE5,
	PCI_CONF_ROM
};

/* Max offset allowed into config space for a particular device. */
static uint64_t max_cfg_size = PCI_CONF_HDR_SIZE;

static uint64_t pcitool_swap_endian(uint64_t data, int size);
static int pcitool_pciex_cfg_access(dev_info_t *dip, pcitool_reg_t *prg,
    boolean_t write_flag);
static int pcitool_cfg_access(dev_info_t *dip, pcitool_reg_t *prg,
    boolean_t write_flag);
static int pcitool_io_access(dev_info_t *dip, pcitool_reg_t *prg,
    boolean_t write_flag);
static int pcitool_mem_access(dev_info_t *dip, pcitool_reg_t *prg,
    uint64_t virt_addr, boolean_t write_flag);
static uint64_t pcitool_map(uint64_t phys_addr, size_t size, size_t *num_pages);
static void pcitool_unmap(uint64_t virt_addr, size_t num_pages);

/* Extern decalrations */
extern int	(*psm_intr_ops)(dev_info_t *, ddi_intr_handle_impl_t *,
		    psm_intr_op_t, int *);

int
pcitool_init(dev_info_t *dip, boolean_t is_pciex)
{
	int instance = ddi_get_instance(dip);

	/* Create pcitool nodes for register access and interrupt routing. */

	if (ddi_create_minor_node(dip, PCI_MINOR_REG, S_IFCHR,
	    PCIHP_AP_MINOR_NUM(instance, PCI_TOOL_REG_MINOR_NUM),
	    DDI_NT_REGACC, 0) != DDI_SUCCESS) {
		return (DDI_FAILURE);
	}

	if (ddi_create_minor_node(dip, PCI_MINOR_INTR, S_IFCHR,
	    PCIHP_AP_MINOR_NUM(instance, PCI_TOOL_INTR_MINOR_NUM),
	    DDI_NT_INTRCTL, 0) != DDI_SUCCESS) {
		ddi_remove_minor_node(dip, PCI_MINOR_REG);
		return (DDI_FAILURE);
	}

	if (is_pciex)
		max_cfg_size = PCIE_CONF_HDR_SIZE;

	return (DDI_SUCCESS);
}

void
pcitool_uninit(dev_info_t *dip)
{
	ddi_remove_minor_node(dip, PCI_MINOR_INTR);
	ddi_remove_minor_node(dip, PCI_MINOR_REG);
}


/* Return the number of interrupts on a pci bus. */
static int
pcitool_intr_get_max_ino(uint32_t *arg, int mode)
{
	uint32_t num_intr = APIC_MAX_VECTOR;

	if (ddi_copyout(&num_intr, arg, sizeof (uint32_t), mode) !=
	    DDI_SUCCESS)
		return (EFAULT);
	else
		return (SUCCESS);
}


/*ARGSUSED*/
static int
pcitool_set_intr(dev_info_t *dip, void *arg, int mode)
{
	ddi_intr_handle_impl_t info_hdl;
	pcitool_intr_set_t iset;
	uint32_t old_cpu;
	int ret, result;
	int rval = SUCCESS;

	if (ddi_copyin(arg, &iset, sizeof (pcitool_intr_set_t), mode) !=
	    DDI_SUCCESS)
		return (EFAULT);

	if (iset.ino > APIC_MAX_VECTOR) {
		rval = EINVAL;
		iset.status = PCITOOL_INVALID_INO;
		goto done_set_intr;
	}

	iset.status = PCITOOL_SUCCESS;

	if ((old_cpu = pci_get_cpu_from_vecirq(iset.ino, IS_VEC)) == -1) {
		iset.status = PCITOOL_IO_ERROR;
		rval = EINVAL;
		goto done_set_intr;
	}

	old_cpu &= ~PSMGI_CPU_USER_BOUND;

	/*
	 * For this locally-declared and used handle, ih_private will contain a
	 * CPU value, not an ihdl_plat_t as used for global interrupt handling.
	 */
	info_hdl.ih_vector = iset.ino;
	info_hdl.ih_private = (void *)(uintptr_t)iset.cpu_id;
	ret = (*psm_intr_ops)(NULL, &info_hdl, PSM_INTR_OP_SET_CPU, &result);

	iset.drvr_version = PCITOOL_DRVR_VERSION;
	if (ret != PSM_SUCCESS) {
		switch (result) {
		case EIO:		/* Error making the change */
			rval = EIO;
			iset.status = PCITOOL_IO_ERROR;
			break;
		case ENXIO:		/* Couldn't convert vector to irq */
			rval = EINVAL;
			iset.status = PCITOOL_INVALID_INO;
			break;
		case EINVAL:		/* CPU out of range */
			rval = EINVAL;
			iset.status = PCITOOL_INVALID_CPUID;
			break;
		}
	}

	/* Return original CPU. */
	iset.cpu_id = old_cpu;

done_set_intr:
	if (ddi_copyout(&iset, arg, sizeof (pcitool_intr_set_t), mode) !=
	    DDI_SUCCESS)
		rval = EFAULT;
	return (rval);
}


/* It is assumed that dip != NULL */
static void
pcitool_get_intr_dev_info(dev_info_t *dip, pcitool_intr_dev_t *devs)
{
	(void) strncpy(devs->driver_name,
	    ddi_driver_name(dip), MAXMODCONFNAME-1);
	devs->driver_name[MAXMODCONFNAME] = '\0';
	(void) ddi_pathname(dip, devs->path);
	devs->dev_inst = ddi_get_instance(dip);
}


/*ARGSUSED*/
static int
pcitool_get_intr(dev_info_t *dip, void *arg, int mode)
{
	/* Array part isn't used here, but oh well... */
	pcitool_intr_get_t partial_iget;
	pcitool_intr_get_t *iget = &partial_iget;
	size_t	iget_kmem_alloc_size = 0;
	uint8_t num_devs_ret;
	int copyout_rval;
	int rval = SUCCESS;
	int circ;
	int i;

	ddi_intr_handle_impl_t info_hdl;
	apic_get_intr_t intr_info;

	/* Read in just the header part, no array section. */
	if (ddi_copyin(arg, &partial_iget, PCITOOL_IGET_SIZE(0), mode) !=
	    DDI_SUCCESS)
		return (EFAULT);

	/* Validate argument. */
	if (partial_iget.ino > APIC_MAX_VECTOR) {
		partial_iget.status = PCITOOL_INVALID_INO;
		partial_iget.num_devs_ret = 0;
		rval = EINVAL;
		goto done_get_intr;
	}

	num_devs_ret = partial_iget.num_devs_ret;
	intr_info.avgi_dip_list = NULL;
	intr_info.avgi_req_flags =
	    PSMGI_REQ_CPUID | PSMGI_REQ_NUM_DEVS | PSMGI_INTRBY_VEC;
	/*
	 * For this locally-declared and used handle, ih_private will contain a
	 * pointer to apic_get_intr_t, not an ihdl_plat_t as used for
	 * global interrupt handling.
	 */
	info_hdl.ih_private = &intr_info;
	info_hdl.ih_vector = partial_iget.ino;

	/* Caller wants device information returned. */
	if (num_devs_ret > 0) {

		intr_info.avgi_req_flags |= PSMGI_REQ_GET_DEVS;

		/*
		 * Allocate room.
		 * If num_devs_ret == 0 iget remains pointing to partial_iget.
		 */
		iget_kmem_alloc_size = PCITOOL_IGET_SIZE(num_devs_ret);
		iget = kmem_alloc(iget_kmem_alloc_size, KM_SLEEP);

		/* Read in whole structure to verify there's room. */
		if (ddi_copyin(arg, iget, iget_kmem_alloc_size, mode) !=
		    SUCCESS) {

			/* Be consistent and just return EFAULT here. */
			kmem_free(iget, iget_kmem_alloc_size);

			return (EFAULT);
		}
	}

	bzero(iget, PCITOOL_IGET_SIZE(num_devs_ret));
	iget->ino = info_hdl.ih_vector;

	/*
	 * Lock device tree branch from the pci root nexus on down if info will
	 * be extracted from dips returned from the tree.
	 */
	if (intr_info.avgi_req_flags & PSMGI_REQ_GET_DEVS) {
		ndi_devi_enter(dip, &circ);
	}

	/* Call psm_intr_ops(PSM_INTR_OP_GET_INTR) to get information. */
	if ((rval = (*psm_intr_ops)(NULL, &info_hdl,
	    PSM_INTR_OP_GET_INTR, NULL)) != PSM_SUCCESS) {
		iget->status = PCITOOL_IO_ERROR;
		iget->num_devs_ret = 0;
		rval = EINVAL;
		goto done_get_intr;
	}

	/*
	 * Fill in the pcitool_intr_get_t to be returned,
	 * with the CPU, num_devs_ret and num_devs.
	 */
	iget->cpu_id = intr_info.avgi_cpu_id & ~PSMGI_CPU_USER_BOUND;

	/* Number of devices returned by apic. */
	iget->num_devs = intr_info.avgi_num_devs;

	/* Device info was returned. */
	if (intr_info.avgi_req_flags & PSMGI_REQ_GET_DEVS) {

		/*
		 * num devs returned is num devs ret by apic,
		 * space permitting.
		 */
		iget->num_devs_ret = min(num_devs_ret, intr_info.avgi_num_devs);

		/*
		 * Loop thru list of dips and extract driver, name and instance.
		 * Fill in the pcitool_intr_dev_t's with this info.
		 */
		for (i = 0; i < iget->num_devs_ret; i++)
			pcitool_get_intr_dev_info(intr_info.avgi_dip_list[i],
			    &iget->dev[i]);

		/* Free kmem_alloc'ed memory of the apic_get_intr_t */
		kmem_free(intr_info.avgi_dip_list,
		    intr_info.avgi_num_devs * sizeof (dev_info_t *));
	}

done_get_intr:

	if (intr_info.avgi_req_flags & PSMGI_REQ_GET_DEVS) {
		ndi_devi_exit(dip, circ);
	}

	iget->drvr_version = PCITOOL_DRVR_VERSION;
	copyout_rval = ddi_copyout(iget, arg,
	    PCITOOL_IGET_SIZE(num_devs_ret), mode);

	if (iget_kmem_alloc_size > 0)
		kmem_free(iget, iget_kmem_alloc_size);

	if (copyout_rval != DDI_SUCCESS)
		rval = EFAULT;

	return (rval);
}


/*
 * Main function for handling interrupt CPU binding requests and queries.
 * Need to implement later
 */
/*ARGSUSED*/
int
pcitool_intr_admn(dev_info_t *dip, void *arg, int cmd, int mode)
{
	int rval;

	switch (cmd) {

	/* Associate a new CPU with a given vector */
	case PCITOOL_DEVICE_SET_INTR:
		rval = pcitool_set_intr(dip, arg, mode);
		break;

	case PCITOOL_DEVICE_GET_INTR:
		rval = pcitool_get_intr(dip, arg, mode);
		break;

	case PCITOOL_DEVICE_NUM_INTR:
		rval = pcitool_intr_get_max_ino(arg, mode);
		break;

	default:
		rval = ENOTSUP;
	}

	return (rval);
}


/*
 * A note about ontrap handling:
 *
 * X86 systems on which this module was tested return FFs instead of bus errors
 * when accessing devices with invalid addresses.  Ontrap handling, which
 * gracefully handles kernel bus errors, is installed anyway, in case future
 * X86 platforms require it.
 */

/*
 * Perform register accesses on the nexus device itself.
 * No explicit PCI nexus device for X86, so not applicable.
 */

/*ARGSUSED*/
int
pcitool_bus_reg_ops(dev_info_t *dip, void *arg, int cmd, int mode)
{
	return (ENOTSUP);
}

/* Swap endianness. */
static uint64_t
pcitool_swap_endian(uint64_t data, int size)
{
	typedef union {
		uint64_t data64;
		uint8_t data8[8];
	} data_split_t;

	data_split_t orig_data;
	data_split_t returned_data;
	int i;

	orig_data.data64 = data;
	returned_data.data64 = 0;

	for (i = 0; i < size; i++) {
		returned_data.data8[i] = orig_data.data8[size - 1 - i];
	}

	return (returned_data.data64);
}


/*
 * Access device.  prg is modified.
 *
 * Extended config space is available only through memory-mapped access.
 * Standard config space on pci express devices is available either way,
 * so do it memory-mapped here too, for simplicity.
 */
/*ARGSUSED*/
static int
pcitool_pciex_cfg_access(dev_info_t *dip, pcitool_reg_t *prg,
    boolean_t write_flag)
{
	int rval = SUCCESS;
	uint64_t virt_addr;
	size_t	num_virt_pages;

	prg->status = PCITOOL_SUCCESS;

	prg->phys_addr = ddi_prop_get_int64(DDI_DEV_T_ANY, dip, 0,
	    "ecfga-base-address", 0);
	if (prg->phys_addr == 0) {
		prg->status = PCITOOL_IO_ERROR;
		return (EIO);
	}

	prg->phys_addr += prg->offset +
	    ((prg->bus_no << PCIEX_REG_BUS_SHIFT) |
	    (prg->dev_no << PCIEX_REG_DEV_SHIFT) |
	    (prg->func_no << PCIEX_REG_FUNC_SHIFT));

	virt_addr = pcitool_map(prg->phys_addr,
	    PCITOOL_ACC_ATTR_SIZE(prg->acc_attr), &num_virt_pages);
	if (virt_addr == NULL) {
		prg->status = PCITOOL_IO_ERROR;
		return (EIO);
	}

	rval = pcitool_mem_access(dip, prg, virt_addr, write_flag);
	pcitool_unmap(virt_addr, num_virt_pages);
	return (rval);
}

/* Access device.  prg is modified. */
/*ARGSUSED*/
static int
pcitool_cfg_access(dev_info_t *dip, pcitool_reg_t *prg, boolean_t write_flag)
{
	int size = PCITOOL_ACC_ATTR_SIZE(prg->acc_attr);
	boolean_t big_endian = PCITOOL_ACC_IS_BIG_ENDIAN(prg->acc_attr);
	int rval = SUCCESS;
	uint64_t local_data;

	/*
	 * NOTE: there is no way to verify whether or not the address is valid.
	 * The put functions return void and the get functions return ff on
	 * error.
	 */
	prg->status = PCITOOL_SUCCESS;

	if (write_flag) {

		if (big_endian) {
			local_data = pcitool_swap_endian(prg->data, size);
		} else {
			local_data = prg->data;
		}

		switch (size) {
		case 1:
			(*pci_putb_func)(prg->bus_no, prg->dev_no,
			    prg->func_no, prg->offset, local_data);
			break;
		case 2:
			(*pci_putw_func)(prg->bus_no, prg->dev_no,
			    prg->func_no, prg->offset, local_data);
			break;
		case 4:
			(*pci_putl_func)(prg->bus_no, prg->dev_no,
			    prg->func_no, prg->offset, local_data);
			break;
		default:
			rval = ENOTSUP;
			prg->status = PCITOOL_INVALID_SIZE;
			break;
		}
	} else {
		switch (size) {
		case 1:
			local_data = (*pci_getb_func)(prg->bus_no, prg->dev_no,
			    prg->func_no, prg->offset);
			break;
		case 2:
			local_data = (*pci_getw_func)(prg->bus_no, prg->dev_no,
			    prg->func_no, prg->offset);
			break;
		case 4:
			local_data = (*pci_getl_func)(prg->bus_no, prg->dev_no,
			    prg->func_no, prg->offset);
			break;
		default:
			rval = ENOTSUP;
			prg->status = PCITOOL_INVALID_SIZE;
			break;
		}

		if (rval == SUCCESS) {
			if (big_endian) {
				prg->data =
				    pcitool_swap_endian(local_data, size);
			} else {
				prg->data = local_data;
			}
		}
	}
	prg->phys_addr = 0;	/* Config space is not memory mapped on X86. */
	return (rval);
}


/*ARGSUSED*/
static int
pcitool_io_access(dev_info_t *dip, pcitool_reg_t *prg, boolean_t write_flag)
{
	int port = (int)prg->phys_addr;
	size_t size = PCITOOL_ACC_ATTR_SIZE(prg->acc_attr);
	boolean_t big_endian = PCITOOL_ACC_IS_BIG_ENDIAN(prg->acc_attr);
	int rval = SUCCESS;
	on_trap_data_t otd;
	uint64_t local_data;


	/*
	 * on_trap works like setjmp.
	 *
	 * A non-zero return here means on_trap has returned from an error.
	 *
	 * A zero return here means that on_trap has just returned from setup.
	 */
	if (on_trap(&otd, OT_DATA_ACCESS)) {
		no_trap();
		if (pcitool_debug)
			prom_printf(
			    "pcitool_mem_access: on_trap caught an error...\n");
		prg->status = PCITOOL_INVALID_ADDRESS;
		return (EFAULT);
	}

	if (write_flag) {

		if (big_endian) {
			local_data = pcitool_swap_endian(prg->data, size);
		} else {
			local_data = prg->data;
		}

		if (pcitool_debug)
			prom_printf("Writing %ld byte(s) to port 0x%x\n",
			    size, port);

		switch (size) {
		case 1:
			outb(port, (uint8_t)local_data);
			break;
		case 2:
			outw(port, (uint16_t)local_data);
			break;
		case 4:
			outl(port, (uint32_t)local_data);
			break;
		default:
			rval = ENOTSUP;
			prg->status = PCITOOL_INVALID_SIZE;
			break;
		}
	} else {
		if (pcitool_debug)
			prom_printf("Reading %ld byte(s) from port 0x%x\n",
			    size, port);

		switch (size) {
		case 1:
			local_data = inb(port);
			break;
		case 2:
			local_data = inw(port);
			break;
		case 4:
			local_data = inl(port);
			break;
		default:
			rval = ENOTSUP;
			prg->status = PCITOOL_INVALID_SIZE;
			break;
		}

		if (rval == SUCCESS) {
			if (big_endian) {
				prg->data =
				    pcitool_swap_endian(local_data, size);
			} else {
				prg->data = local_data;
			}
		}
	}

	no_trap();
	return (rval);
}

/*ARGSUSED*/
static int
pcitool_mem_access(dev_info_t *dip, pcitool_reg_t *prg, uint64_t virt_addr,
	boolean_t write_flag)
{
	size_t size = PCITOOL_ACC_ATTR_SIZE(prg->acc_attr);
	boolean_t big_endian = PCITOOL_ACC_IS_BIG_ENDIAN(prg->acc_attr);
	int rval = DDI_SUCCESS;
	on_trap_data_t otd;
	uint64_t local_data;

	/*
	 * on_trap works like setjmp.
	 *
	 * A non-zero return here means on_trap has returned from an error.
	 *
	 * A zero return here means that on_trap has just returned from setup.
	 */
	if (on_trap(&otd, OT_DATA_ACCESS)) {
		no_trap();
		if (pcitool_debug)
			prom_printf(
			    "pcitool_mem_access: on_trap caught an error...\n");
		prg->status = PCITOOL_INVALID_ADDRESS;
		return (EFAULT);
	}

	if (write_flag) {

		if (big_endian) {
			local_data = pcitool_swap_endian(prg->data, size);
		} else {
			local_data = prg->data;
		}

		switch (size) {
		case 1:
			*((uint8_t *)(uintptr_t)virt_addr) = local_data;
			break;
		case 2:
			*((uint16_t *)(uintptr_t)virt_addr) = local_data;
			break;
		case 4:
			*((uint32_t *)(uintptr_t)virt_addr) = local_data;
			break;
		case 8:
			*((uint64_t *)(uintptr_t)virt_addr) = local_data;
			break;
		default:
			rval = ENOTSUP;
			prg->status = PCITOOL_INVALID_SIZE;
			break;
		}
	} else {
		switch (size) {
		case 1:
			local_data = *((uint8_t *)(uintptr_t)virt_addr);
			break;
		case 2:
			local_data = *((uint16_t *)(uintptr_t)virt_addr);
			break;
		case 4:
			local_data = *((uint32_t *)(uintptr_t)virt_addr);
			break;
		case 8:
			local_data = *((uint64_t *)(uintptr_t)virt_addr);
			break;
		default:
			rval = ENOTSUP;
			prg->status = PCITOOL_INVALID_SIZE;
			break;
		}

		if (rval == SUCCESS) {
			if (big_endian) {
				prg->data =
				    pcitool_swap_endian(local_data, size);
			} else {
				prg->data = local_data;
			}
		}
	}

	no_trap();
	return (rval);
}

/*
 * Map up to 2 pages which contain the address we want to access.
 *
 * Mapping should span no more than 8 bytes.  With X86 it is possible for an
 * 8 byte value to start on a 4 byte boundary, so it can cross a page boundary.
 * We'll never have to map more than two pages.
 */

static uint64_t
pcitool_map(uint64_t phys_addr, size_t size, size_t *num_pages)
{

	uint64_t page_base = phys_addr & ~MMU_PAGEOFFSET;
	uint64_t offset = phys_addr & MMU_PAGEOFFSET;
	void *virt_base;
	uint64_t returned_addr;

	if (pcitool_debug)
		prom_printf("pcitool_map: Called with PA:0x%p\n",
		    (uint8_t *)(uintptr_t)phys_addr);

	*num_pages = 1;

	/* Desired mapping would span more than two pages. */
	if ((offset + size) > (MMU_PAGESIZE * 2)) {
		if (pcitool_debug)
			prom_printf("boundary violation: "
			    "offset:0x%" PRIx64 ", size:%ld, pagesize:0x%lx\n",
			    offset, (uintptr_t)size, (uintptr_t)MMU_PAGESIZE);
		return (NULL);

	} else if ((offset + size) > MMU_PAGESIZE) {
		(*num_pages)++;
	}

	/* Get page(s) of virtual space. */
	virt_base = vmem_alloc(heap_arena, ptob(*num_pages), VM_NOSLEEP);
	if (virt_base == NULL) {
		if (pcitool_debug)
			prom_printf("Couldn't get virtual base address.\n");
		return (NULL);
	}

	if (pcitool_debug)
		prom_printf("Got base virtual address:0x%p\n", virt_base);

	/* Now map the allocated virtual space to the physical address. */
	hat_devload(kas.a_hat, virt_base, mmu_ptob(*num_pages),
	    mmu_btop(page_base), PROT_READ | PROT_WRITE | HAT_STRICTORDER,
	    HAT_LOAD_LOCK);

	returned_addr = ((uintptr_t)(virt_base)) + offset;

	if (pcitool_debug)
		prom_printf("pcitool_map: returning VA:0x%p\n",
		    (void *)(uintptr_t)returned_addr);

	return (returned_addr);
}

/* Unmap the mapped page(s). */
static void
pcitool_unmap(uint64_t virt_addr, size_t num_pages)
{
	void *base_virt_addr = (void *)(uintptr_t)(virt_addr & ~MMU_PAGEOFFSET);

	hat_unload(kas.a_hat, base_virt_addr, ptob(num_pages),
	    HAT_UNLOAD_UNLOCK);
	vmem_free(heap_arena, base_virt_addr, ptob(num_pages));
}


/* Perform register accesses on PCI leaf devices. */
int
pcitool_dev_reg_ops(dev_info_t *dip, void *arg, int cmd, int mode)
{
	boolean_t	write_flag = B_FALSE;
	int		rval = 0;
	pcitool_reg_t	prg;
	uint8_t		size;

	uint64_t	base_addr;
	uint64_t	virt_addr;
	size_t		num_virt_pages;

	switch (cmd) {
	case (PCITOOL_DEVICE_SET_REG):
		write_flag = B_TRUE;

	/*FALLTHRU*/
	case (PCITOOL_DEVICE_GET_REG):
		if (pcitool_debug)
			prom_printf("pci_dev_reg_ops set/get reg\n");
		if (ddi_copyin(arg, &prg, sizeof (pcitool_reg_t), mode) !=
		    DDI_SUCCESS) {
			if (pcitool_debug)
				prom_printf("Error reading arguments\n");
			return (EFAULT);
		}

		if (prg.barnum >= (sizeof (pci_bars) / sizeof (pci_bars[0]))) {
			prg.status = PCITOOL_OUT_OF_RANGE;
			rval = EINVAL;
			goto done_reg;
		}

		if (pcitool_debug)
			prom_printf("raw bus:0x%x, dev:0x%x, func:0x%x\n",
			    prg.bus_no, prg.dev_no, prg.func_no);
		/* Validate address arguments of bus / dev / func */
		if (((prg.bus_no &
		    (PCI_REG_BUS_M >> PCI_REG_BUS_SHIFT)) !=
		    prg.bus_no) ||
		    ((prg.dev_no &
		    (PCI_REG_DEV_M >> PCI_REG_DEV_SHIFT)) !=
		    prg.dev_no) ||
		    ((prg.func_no &
		    (PCI_REG_FUNC_M >> PCI_REG_FUNC_SHIFT)) !=
		    prg.func_no)) {
			prg.status = PCITOOL_INVALID_ADDRESS;
			rval = EINVAL;
			goto done_reg;
		}

		size = PCITOOL_ACC_ATTR_SIZE(prg.acc_attr);

		/* Proper config space desired. */
		if (prg.barnum == 0) {

			if (pcitool_debug)
				prom_printf(
				    "config access: offset:0x%" PRIx64 ", "
				    "phys_addr:0x%" PRIx64 "\n",
				    prg.offset, prg.phys_addr);

			if (prg.offset >= max_cfg_size) {
				prg.status = PCITOOL_OUT_OF_RANGE;
				rval = EINVAL;
				goto done_reg;
			}

			/*
			 * Access device.  prg is modified.
			 * First, check for AMD northbridges for I/O access
			 * (This fix will move in future to pcitool user-land)
			 * Next, check for PCIe devices and do
			 * memory-mapped access
			 * Lastly, check for PCI devices and do I/O access
			 */
			if ((prg.bus_no == 0) &&
			    (prg.dev_no >= 0x18) &&
			    (prg.dev_no < (0x18 + ncpus))) {
				if (cpuid_getvendor(CPU) == X86_VENDOR_AMD)
					rval = pcitool_cfg_access(dip, &prg,
					    write_flag);
			} else if (max_cfg_size == PCIE_CONF_HDR_SIZE)
				rval = pcitool_pciex_cfg_access(dip, &prg,
				    write_flag);
			else
				rval = pcitool_cfg_access(dip, &prg,
				    write_flag);

			if (pcitool_debug)
				prom_printf(
				    "config access: data:0x%" PRIx64 "\n",
				    prg.data);

		/* IO/ MEM/ MEM64 space. */
		} else {

			pcitool_reg_t	prg2;
			bcopy(&prg, &prg2, sizeof (pcitool_reg_t));

			/*
			 * Translate BAR number into offset of the BAR in
			 * the device's config space.
			 */
			prg2.offset = pci_bars[prg2.barnum];
			prg2.acc_attr =
			    PCITOOL_ACC_ATTR_SIZE_4 | PCITOOL_ACC_ATTR_ENDN_LTL;

			if (pcitool_debug)
				prom_printf(
				    "barnum:%d, bar_offset:0x%" PRIx64 "\n",
				    prg2.barnum, prg2.offset);
			/*
			 * Get Bus Address Register (BAR) from config space.
			 * prg2.offset is the offset into config space of the
			 * BAR desired.  prg.status is modified on error.
			 */
			rval = pcitool_cfg_access(dip, &prg2, B_FALSE);
			if (rval != SUCCESS) {
				if (pcitool_debug)
					prom_printf("BAR access failed\n");
				prg.status = prg2.status;
				goto done_reg;
			}
			/*
			 * Reference proper PCI space based on the BAR.
			 * If 64 bit MEM space, need to load other half of the
			 * BAR first.
			 */

			if (pcitool_debug)
				prom_printf("bar returned is 0x%" PRIx64 "\n",
				    prg2.data);
			if (!prg2.data) {
				if (pcitool_debug)
					prom_printf("BAR data == 0\n");
				rval = EINVAL;
				prg.status = PCITOOL_INVALID_ADDRESS;
				goto done_reg;
			}
			if (prg2.data == 0xffffffff) {
				if (pcitool_debug)
					prom_printf("BAR data == -1\n");
				rval = EINVAL;
				prg.status = PCITOOL_INVALID_ADDRESS;
				goto done_reg;
			}

			/*
			 * BAR has bits saying this space is IO space, unless
			 * this is the ROM address register.
			 */
			if (((PCI_BASE_SPACE_M & prg2.data) ==
			    PCI_BASE_SPACE_IO) &&
			    (prg2.offset != PCI_CONF_ROM)) {
				if (pcitool_debug)
					prom_printf("IO space\n");

				prg2.data &= PCI_BASE_IO_ADDR_M;
				prg.phys_addr = prg2.data + prg.offset;

				rval = pcitool_io_access(dip, &prg, write_flag);
				if ((rval != SUCCESS) && (pcitool_debug))
					prom_printf("IO access failed\n");

				goto done_reg;


			/*
			 * BAR has bits saying this space is 64 bit memory
			 * space, unless this is the ROM address register.
			 *
			 * The 64 bit address stored in two BAR cells is not
			 * necessarily aligned on an 8-byte boundary.
			 * Need to keep the first 4 bytes read,
			 * and do a separate read of the high 4 bytes.
			 */

			} else if ((PCI_BASE_TYPE_ALL & prg2.data) &&
			    (prg2.offset != PCI_CONF_ROM)) {

				uint32_t low_bytes =
				    (uint32_t)(prg2.data & ~PCI_BASE_TYPE_ALL);

				/*
				 * Don't try to read the next 4 bytes
				 * past the end of BARs.
				 */
				if (prg2.offset >= PCI_CONF_BASE5) {
					prg.status = PCITOOL_OUT_OF_RANGE;
					rval = EIO;
					goto done_reg;
				}

				/*
				 * Access device.
				 * prg2.status is modified on error.
				 */
				prg2.offset += 4;
				rval = pcitool_cfg_access(dip, &prg2, B_FALSE);
				if (rval != SUCCESS) {
					prg.status = prg2.status;
					goto done_reg;
				}

				if (prg2.data == 0xffffffff) {
					prg.status = PCITOOL_INVALID_ADDRESS;
					prg.status = EFAULT;
					goto done_reg;
				}

				prg2.data = (prg2.data << 32) + low_bytes;
				if (pcitool_debug)
					prom_printf(
					    "64 bit mem space.  "
					    "64-bit bar is 0x%" PRIx64 "\n",
					    prg2.data);

			/* Mem32 space, including ROM */
			} else {

				if (prg2.offset == PCI_CONF_ROM) {
					if (pcitool_debug)
						prom_printf(
						    "Additional ROM "
						    "checking\n");
					/* Can't write to ROM */
					if (write_flag) {
						prg.status = PCITOOL_ROM_WRITE;
						rval = EIO;
						goto done_reg;

					/* ROM disabled for reading */
					} else if (!(prg2.data & 0x00000001)) {
						prg.status =
						    PCITOOL_ROM_DISABLED;
						rval = EIO;
						goto done_reg;
					}
				}

				if (pcitool_debug)
					prom_printf("32 bit mem space\n");
			}

			/* Common code for all IO/MEM range spaces. */

			base_addr = prg2.data;
			if (pcitool_debug)
				prom_printf(
				    "addr portion of bar is 0x%" PRIx64 ", "
				    "base=0x%" PRIx64 ", "
				    "offset:0x%" PRIx64 "\n",
				    prg2.data, base_addr, prg.offset);
			/*
			 * Use offset provided by caller to index into
			 * desired space, then access.
			 * Note that prg.status is modified on error.
			 */
			prg.phys_addr = base_addr + prg.offset;

			virt_addr = pcitool_map(prg.phys_addr, size,
			    &num_virt_pages);
			if (virt_addr == NULL) {
				prg.status = PCITOOL_IO_ERROR;
				rval = EIO;
				goto done_reg;
			}

			rval = pcitool_mem_access(dip, &prg, virt_addr,
			    write_flag);
			pcitool_unmap(virt_addr, num_virt_pages);
		}
done_reg:
		if (ddi_copyout(&prg, arg, sizeof (pcitool_reg_t), mode) !=
		    DDI_SUCCESS) {
			if (pcitool_debug)
				prom_printf("Error returning arguments.\n");
			rval = EFAULT;
		}
		break;
	default:
		rval = ENOTTY;
		break;
	}
	return (rval);
}
