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

#pragma ident	"@(#)pcie_pci.c	1.7	06/07/24 SMI"


/*
 * PCI-E to PCI bus bridge nexus driver
 */

#include <sys/conf.h>
#include <sys/kmem.h>
#include <sys/debug.h>
#include <sys/modctl.h>
#include <sys/autoconf.h>
#include <sys/ddi_impldefs.h>
#include <sys/pci.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/ddifm.h>
#include <sys/ndifm.h>
#include <sys/fm/util.h>
#include <sys/fm/protocol.h>
#include <sys/pcie.h>
#include <sys/pcie_impl.h>
#include <sys/hotplug/pci/pcihp.h>
#include <sys/hotplug/pci/pciehpc.h>
#include <io/pciex/pcie_error.h>
#include <io/pciex/pcie_nvidia.h>

#ifdef DEBUG
static int pepb_debug = 0;
#define	PEPB_DEBUG(args)	if (pepb_debug) cmn_err args
#else
#define	PEPB_DEBUG(args)
#endif

/*
 * interfaces from misc/pcie
 */
static int	pepb_bus_map(dev_info_t *, dev_info_t *, ddi_map_req_t *, off_t,
		    off_t, caddr_t *);
static int	pepb_ctlops(dev_info_t *, dev_info_t *, ddi_ctl_enum_t, void *,
		    void *);
static int	pepb_fm_init(dev_info_t *, dev_info_t *, int,
		    ddi_iblock_cookie_t *);

static int	pepb_fm_callback(dev_info_t *, ddi_fm_error_t *, const void *);

struct bus_ops pepb_bus_ops = {
	BUSO_REV,
	pepb_bus_map,
	0,
	0,
	0,
	i_ddi_map_fault,
	ddi_dma_map,
	ddi_dma_allochdl,
	ddi_dma_freehdl,
	ddi_dma_bindhdl,
	ddi_dma_unbindhdl,
	ddi_dma_flush,
	ddi_dma_win,
	ddi_dma_mctl,
	pepb_ctlops,
	ddi_bus_prop_op,
	0,		/* (*bus_get_eventcookie)();	*/
	0,		/* (*bus_add_eventcall)();	*/
	0,		/* (*bus_remove_eventcall)();	*/
	0,		/* (*bus_post_event)();		*/
	0,		/* (*bus_intr_ctl)();		*/
	0,		/* (*bus_config)(); 		*/
	0,		/* (*bus_unconfig)(); 		*/
	pepb_fm_init,	/* (*bus_fm_init)(); 		*/
	NULL,		/* (*bus_fm_fini)(); 		*/
	NULL,		/* (*bus_fm_access_enter)(); 	*/
	NULL,		/* (*bus_fm_access_exit)(); 	*/
	NULL,		/* (*bus_power)(); 	*/
	i_ddi_intr_ops	/* (*bus_intr_op)(); 		*/
};

/*
 * The goal here is to leverage off of the pcihp.c source without making
 * changes to it.  Call into it's cb_ops directly if needed.
 */
static int	pepb_open(dev_t *, int, int, cred_t *);
static int	pepb_close(dev_t, int, int, cred_t *);
static int	pepb_ioctl(dev_t, int, intptr_t, int, cred_t *, int *);
static int	pepb_prop_op(dev_t, dev_info_t *, ddi_prop_op_t, int, char *,
		    caddr_t, int *);
static int	pepb_info(dev_info_t *, ddi_info_cmd_t, void *, void **);

struct cb_ops pepb_cb_ops = {
	pepb_open,			/* open */
	pepb_close,			/* close */
	nodev,				/* strategy */
	nodev,				/* print */
	nodev,				/* dump */
	nodev,				/* read */
	nodev,				/* write */
	pepb_ioctl,			/* ioctl */
	nodev,				/* devmap */
	nodev,				/* mmap */
	nodev,				/* segmap */
	nochpoll,			/* poll */
	pepb_prop_op,			/* cb_prop_op */
	NULL,				/* streamtab */
	D_NEW | D_MP | D_HOTPLUG,	/* Driver compatibility flag */
	CB_REV,				/* rev */
	nodev,				/* int (*cb_aread)() */
	nodev				/* int (*cb_awrite)() */
};

static int	pepb_probe(dev_info_t *);
static int	pepb_attach(dev_info_t *devi, ddi_attach_cmd_t cmd);
static int	pepb_detach(dev_info_t *devi, ddi_detach_cmd_t cmd);
static int	pepb_check_slot_disabled(dev_info_t *dip);

struct dev_ops pepb_ops = {
	DEVO_REV,		/* devo_rev */
	0,			/* refcnt  */
	pepb_info,		/* info */
	nulldev,		/* identify */
	pepb_probe,		/* probe */
	pepb_attach,		/* attach */
	pepb_detach,		/* detach */
	nulldev,		/* reset */
	&pepb_cb_ops,		/* driver operations */
	&pepb_bus_ops		/* bus operations */
};

/*
 * Module linkage information for the kernel.
 */

static struct modldrv modldrv = {
	&mod_driverops, /* Type of module */
	"PCIe to PCI nexus driver 1.7",
	&pepb_ops,	/* driver ops */
};

static struct modlinkage modlinkage = {
	MODREV_1,
	(void *)&modldrv,
	NULL
};


/*
 * soft state pointer and structure template:
 */
static void *pepb_state;

typedef struct {
	dev_info_t *dip;

	/*
	 * cpr support:
	 */
#define	PCI_MAX_DEVICES		32
#define	PCI_MAX_FUNCTIONS	8
#define	PCI_MAX_CHILDREN	PCI_MAX_DEVICES * PCI_MAX_FUNCTIONS
	uint_t	config_state_index;
	struct {
		dev_info_t	*dip;
		ushort_t	command;
		uchar_t		cache_line_size;
		uchar_t		latency_timer;
		uchar_t		header_type;
		uchar_t		sec_latency_timer;
		ushort_t	bridge_control;
	} config_state[PCI_MAX_CHILDREN];

	/*
	 * hot plug support
	 */
	int			inband_hpc;	/* inband HPC type */

	/*
	 * interrupt support
	 */
	ddi_intr_handle_t	*htable;	/* interrupt handles */
	int			htable_size;	/* htable size */
	int			intr_count;	/* Num of Intr */
	uint_t			intr_priority;	/* Intr Priority */
	int			intr_type;	/* (MSI | FIXED) */
	uint32_t		soft_state;	/* soft state flags */
	kmutex_t		pepb_mutex;	/* Mutex for this ctrl */
	kmutex_t		pepb_err_mutex;	/* Error handling mutex */
	kmutex_t		pepb_peek_poke_mutex;
	int			pepb_fmcap;
	ddi_iblock_cookie_t	pepb_fm_ibc;
	int			port_type;
} pepb_devstate_t;

/* soft state flags */
#define	PEPB_SOFT_STATE_INIT_HTABLE	0x01	/* htable kmem_alloced */
#define	PEPB_SOFT_STATE_INIT_ALLOC	0x02	/* ddi_intr_alloc called */
#define	PEPB_SOFT_STATE_INIT_HANDLER	0x04	/* ddi_intr_add_handler done */
#define	PEPB_SOFT_STATE_INIT_ENABLE	0x08	/* ddi_intr_enable called */
#define	PEPB_SOFT_STATE_INIT_BLOCK	0x10	/* ddi_intr_block_enable done */
#define	PEPB_SOFT_STATE_INIT_MUTEX	0x20	/* mutex initialized */

/* default interrupt priority for all interrupts (hotplug or non-hotplug */
#define	PEPB_INTR_PRI	1

/* flag to turn on MSI support */
static int pepb_enable_msi = 1;
/* panic on unknown flag, defaulted to on */
int pepb_panic_unknown = 1;
int pepb_panic_fatal = 1;

extern errorq_t *pci_target_queue;

/*
 * forward function declarations:
 */
static void	pepb_uninitchild(dev_info_t *);
static int 	pepb_initchild(dev_info_t *child);
static void 	pepb_save_config_regs(pepb_devstate_t *pepb_p);
static void	pepb_restore_config_regs(pepb_devstate_t *pepb_p);
static int	pepb_pcie_device_type(dev_info_t *dip, int *port_type);
static int	pepb_pcie_port_type(dev_info_t *dip,
			ddi_acc_handle_t config_handle);

/* interrupt related declarations */
static uint_t	pepb_intx_intr(caddr_t arg, caddr_t arg2);
static uint_t	pepb_pwr_msi_intr(caddr_t arg, caddr_t arg2);
static uint_t	pepb_err_msi_intr(caddr_t arg, caddr_t arg2);
static int	pepb_is_nvidia_root_port(dev_info_t *);
static int	pepb_intr_init(pepb_devstate_t *pepb_p, int intr_type);
static void	pepb_intr_fini(pepb_devstate_t *pepb_p);

int
_init(void)
{
	int e;

	if ((e = ddi_soft_state_init(&pepb_state, sizeof (pepb_devstate_t),
	    1)) == 0 && (e = mod_install(&modlinkage)) != 0)
		ddi_soft_state_fini(&pepb_state);
	return (e);
}

int
_fini(void)
{
	int e;

	if ((e = mod_remove(&modlinkage)) == 0) {
		/*
		 * Destroy pci_target_queue, and set it to NULL.
		 */
		if (pci_target_queue)
			errorq_destroy(pci_target_queue);
		pci_target_queue = NULL;
		ddi_soft_state_fini(&pepb_state);
	}
	return (e);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

/*ARGSUSED*/
static int
pepb_probe(dev_info_t *devi)
{
	return (DDI_PROBE_SUCCESS);
}

static int
pepb_attach(dev_info_t *devi, ddi_attach_cmd_t cmd)
{
	int			instance;
	int			intr_types;
	char			device_type[8];
	pepb_devstate_t		*pepb;
	ddi_acc_handle_t	config_handle;

	switch (cmd) {
	case DDI_RESUME:

		/*
		 * Get the soft state structure for the bridge.
		 */
		pepb = ddi_get_soft_state(pepb_state, ddi_get_instance(devi));
		pepb_restore_config_regs(pepb);
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);

	case DDI_ATTACH:
		break;
	}

	/*
	 * If PCIE_LINKCTL_LINK_DISABLE bit in the PCIe Config
	 * Space (PCIe Capability Link Control Register) is set,
	 * then do not bind the driver.
	 */
	if (pepb_check_slot_disabled(devi) == 1)
		return (DDI_FAILURE);

	/*
	 * Allocate and get soft state structure.
	 */
	instance = ddi_get_instance(devi);
	if (ddi_soft_state_zalloc(pepb_state, instance) != DDI_SUCCESS)
		return (DDI_FAILURE);
	pepb = ddi_get_soft_state(pepb_state, instance);
	pepb->dip = devi;

	/*
	 * initalise fma support before we start accessing config space
	 */
	pci_targetq_init();
	pepb->pepb_fmcap = DDI_FM_EREPORT_CAPABLE | DDI_FM_ERRCB_CAPABLE |
	    DDI_FM_ACCCHK_CAPABLE | DDI_FM_DMACHK_CAPABLE;
	ddi_fm_init(devi, &pepb->pepb_fmcap, &pepb->pepb_fm_ibc);

	mutex_init(&pepb->pepb_err_mutex, NULL, MUTEX_DRIVER,
	    (void *)pepb->pepb_fm_ibc);
	mutex_init(&pepb->pepb_peek_poke_mutex, NULL, MUTEX_DRIVER,
	    (void *)pepb->pepb_fm_ibc);

	if (pepb->pepb_fmcap & (DDI_FM_ERRCB_CAPABLE|DDI_FM_EREPORT_CAPABLE))
		pci_ereport_setup(devi);

	if (pepb->pepb_fmcap & DDI_FM_ERRCB_CAPABLE)
		ddi_fm_handler_register(devi, pepb_fm_callback, NULL);

	/*
	 * Make sure the "device_type" property exists.
	 */
	if (pepb_pcie_device_type(devi, &pepb->port_type) == DDI_SUCCESS)
		(void) strcpy(device_type, "pciex");
	else
		(void) strcpy(device_type, "pci");
	(void) ddi_prop_update_string(DDI_DEV_T_NONE, devi,
	    "device_type", device_type);

	/* probe for inband HPC */
	pepb->inband_hpc = ddi_prop_get_int(DDI_DEV_T_ANY, devi,
	    DDI_PROP_DONTPASS, "pci-hotplug-type", INBAND_HPC_NONE);

	/*
	 * Initialize interrupt handlers.
	 */
	if (ddi_intr_get_supported_types(devi, &intr_types) != DDI_SUCCESS)
		goto next_step;

	PEPB_DEBUG((CE_NOTE, "%s#%d: intr_types = 0x%x\n",
	    ddi_driver_name(devi), ddi_get_instance(devi), intr_types));

	if (pepb_enable_msi && (intr_types & DDI_INTR_TYPE_MSI) &&
	    pepb_is_nvidia_root_port(devi) == DDI_SUCCESS) {
		if (pepb_intr_init(pepb, DDI_INTR_TYPE_MSI) == DDI_SUCCESS)
			goto next_step;
		else
			PEPB_DEBUG((CE_WARN,
			    "%s#%d: Unable to attach MSI handler",
			    ddi_driver_name(devi), ddi_get_instance(devi)));
	}

	/*
	 * Only register hotplug interrupts for now.
	 * Check if device supports PCIe hotplug or not?
	 * If yes, register fixed interrupts if ILINE is valid.
	 */
	if (pepb->inband_hpc == INBAND_HPC_PCIE) {
		uint8_t iline;

		(void) pci_config_setup(devi, &config_handle);
		iline = pci_config_get8(config_handle, PCI_CONF_ILINE);
		pci_config_teardown(&config_handle);

		if (iline == 0 || iline > 15)
			goto next_step;

		if (pepb_intr_init(pepb, DDI_INTR_TYPE_FIXED) != DDI_SUCCESS)
			PEPB_DEBUG((CE_WARN,
			    "%s#%d: Unable to attach INTx handler",
			    ddi_driver_name(devi), ddi_get_instance(devi)));
	}

next_step:
	/*
	 * Initialize hotplug support on this bus. At minimum
	 * (for non hotplug bus) this would create ":devctl" minor
	 * node to support DEVCTL_DEVICE_* and DEVCTL_BUS_* ioctls
	 * to this bus.
	 */
	if (pcihp_init(devi) != DDI_SUCCESS)
		cmn_err(CE_WARN, "Failed to setup hotplug framework");
	else {
		/*
		 * If there is an inband PCI-E HPC then initialize it.
		 * The failure is not considered fatal for the system
		 * so log the message and ignore the failure.
		 */
		if (pepb->inband_hpc == INBAND_HPC_PCIE &&
		    pciehpc_init(devi, NULL) != DDI_SUCCESS) {
		    pepb->inband_hpc = INBAND_HPC_NONE;
		    cmn_err(CE_CONT, "!Failed to initialize inband hotplug "
			"controller");
		}
	}

	ddi_report_dev(devi);
	return (DDI_SUCCESS);
}

static int
pepb_detach(dev_info_t *devi, ddi_detach_cmd_t cmd)
{
	pepb_devstate_t *pepb;

	switch (cmd) {
	case DDI_SUSPEND:
		pepb = ddi_get_soft_state(pepb_state, ddi_get_instance(devi));
		pepb_save_config_regs(pepb);
		return (DDI_SUCCESS);

	case DDI_DETACH:
		break;

	default:
		return (DDI_FAILURE);
	}

	(void) ddi_prop_remove(DDI_DEV_T_NONE, devi, "device_type");
	pepb = ddi_get_soft_state(pepb_state, ddi_get_instance(devi));

	/* remove interrupt handlers */
	pepb_intr_fini(pepb);

	/* uninitialize inband PCI-E HPC if present */
	if (pepb->inband_hpc == INBAND_HPC_PCIE)
		(void) pciehpc_uninit(devi);

	/*
	 * Uninitialize hotplug support on this bus.
	 */
	(void) pcihp_uninit(devi);
	if (pepb->pepb_fmcap & DDI_FM_ERRCB_CAPABLE)
		ddi_fm_handler_unregister(devi);

	if (pepb->pepb_fmcap & (DDI_FM_ERRCB_CAPABLE|DDI_FM_EREPORT_CAPABLE))
		pci_ereport_teardown(devi);

	mutex_destroy(&pepb->pepb_err_mutex);
	mutex_destroy(&pepb->pepb_peek_poke_mutex);
	ddi_fm_fini(devi);

	/*
	 * And finally free the per-pci soft state.
	 */
	ddi_soft_state_free(pepb_state, ddi_get_instance(devi));

	return (DDI_SUCCESS);
}

static int
pepb_bus_map(dev_info_t *dip, dev_info_t *rdip, ddi_map_req_t *mp,
    off_t offset, off_t len, caddr_t *vaddrp)
{
	dev_info_t *pdip;

	pdip = (dev_info_t *)DEVI(dip)->devi_parent;
	return ((DEVI(pdip)->devi_ops->devo_bus_ops->bus_map)(pdip, rdip, mp,
	    offset, len, vaddrp));
}

static int
pepb_ctlops(dev_info_t *dip, dev_info_t *rdip, ddi_ctl_enum_t ctlop,
    void *arg, void *result)
{
	pci_regspec_t *drv_regp;
	int	reglen;
	int	rn;
	int	totreg;
	pepb_devstate_t		*pepb;

	switch (ctlop) {
	case DDI_CTLOPS_REPORTDEV:
		if (rdip == (dev_info_t *)0)
			return (DDI_FAILURE);
		cmn_err(CE_CONT, "?PCIE-device: %s@%s, %s%d\n",
		    ddi_node_name(rdip), ddi_get_name_addr(rdip),
		    ddi_driver_name(rdip),
		    ddi_get_instance(rdip));
		return (DDI_SUCCESS);

	case DDI_CTLOPS_INITCHILD:
		return (pepb_initchild((dev_info_t *)arg));

	case DDI_CTLOPS_UNINITCHILD:
		pepb_uninitchild((dev_info_t *)arg);
		return (DDI_SUCCESS);

	case DDI_CTLOPS_SIDDEV:
		return (DDI_SUCCESS);

	case DDI_CTLOPS_REGSIZE:
	case DDI_CTLOPS_NREGS:
		if (rdip == (dev_info_t *)0)
			return (DDI_FAILURE);
		break;

	case DDI_CTLOPS_PEEK:
	case DDI_CTLOPS_POKE:
		pepb = ddi_get_soft_state(pepb_state, ddi_get_instance(dip));
		if (pepb->port_type != PCIE_PCIECAP_DEV_TYPE_ROOT)
			return (ddi_ctlops(dip, rdip, ctlop, arg, result));
		return (pci_peekpoke_check(dip, rdip, ctlop, arg, result,
		    ddi_ctlops, &pepb->pepb_err_mutex,
		    &pepb->pepb_peek_poke_mutex));

	default:
		return (ddi_ctlops(dip, rdip, ctlop, arg, result));
	}

	*(int *)result = 0;
	if (ddi_getlongprop(DDI_DEV_T_ANY, rdip,
	    DDI_PROP_DONTPASS | DDI_PROP_CANSLEEP, "reg", (caddr_t)&drv_regp,
	    &reglen) != DDI_SUCCESS)
		return (DDI_FAILURE);

	totreg = reglen / sizeof (pci_regspec_t);
	if (ctlop == DDI_CTLOPS_NREGS)
		*(int *)result = totreg;
	else if (ctlop == DDI_CTLOPS_REGSIZE) {
		rn = *(int *)arg;
		if (rn >= totreg) {
			kmem_free(drv_regp, reglen);
			return (DDI_FAILURE);
		}
		*(off_t *)result = drv_regp[rn].pci_size_low;
	}

	kmem_free(drv_regp, reglen);
	return (DDI_SUCCESS);
}

static int
pepb_name_child(dev_info_t *child, char *name, int namelen)
{
	pci_regspec_t *pci_rp;
	uint_t slot, func;
	char **unit_addr;
	uint_t n;

	/*
	 * For .conf nodes, use unit-address property as name
	 */
	if (ndi_dev_is_persistent_node(child) == 0) {
		if (ddi_prop_lookup_string_array(DDI_DEV_T_ANY, child,
		    DDI_PROP_DONTPASS, "unit-address", &unit_addr, &n) !=
		    DDI_PROP_SUCCESS) {
			cmn_err(CE_WARN,
			    "cannot find unit-address in %s.conf",
			    ddi_driver_name(child));
			return (DDI_FAILURE);
		}
		if (n != 1 || *unit_addr == NULL || **unit_addr == 0) {
			cmn_err(CE_WARN, "unit-address property in %s.conf"
			    " not well-formed", ddi_driver_name(child));
			ddi_prop_free(unit_addr);
			return (DDI_SUCCESS);
		}
		(void) snprintf(name, namelen, "%s", *unit_addr);
		ddi_prop_free(unit_addr);
		return (DDI_SUCCESS);
	}

	/* get child "reg" property */
	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, child,
	    DDI_PROP_DONTPASS, "reg", (int **)&pci_rp, &n) != DDI_SUCCESS) {
		return (DDI_FAILURE);
	}

	/* copy the device identifications */
	slot = PCI_REG_DEV_G(pci_rp->pci_phys_hi);
	func = PCI_REG_FUNC_G(pci_rp->pci_phys_hi);

	if (func != 0)
		(void) snprintf(name, namelen, "%x,%x", slot, func);
	else
		(void) snprintf(name, namelen, "%x", slot);

	ddi_prop_free(pci_rp);
	return (DDI_SUCCESS);
}

static int
pepb_initchild(dev_info_t *child)
{
	struct ddi_parent_private_data *pdptr;
	char name[MAXNAMELEN];

	if (pepb_name_child(child, name, MAXNAMELEN) != DDI_SUCCESS)
		return (DDI_FAILURE);
	ddi_set_name_addr(child, name);

	/*
	 * Pseudo nodes indicate a prototype node with per-instance
	 * properties to be merged into the real h/w device node.
	 * The interpretation of the unit-address is DD[,F]
	 * where DD is the device id and F is the function.
	 */
	if (ndi_dev_is_persistent_node(child) == 0) {
		extern int pci_allow_pseudo_children;

		ddi_set_parent_data(child, NULL);

		/*
		 * Try to merge the properties from this prototype
		 * node into real h/w nodes.
		 */
		if (ndi_merge_node(child, pepb_name_child) != DDI_SUCCESS) {
			/*
			 * Merged ok - return failure to remove the node.
			 */
			ddi_set_name_addr(child, NULL);
			return (DDI_FAILURE);
		}

		/* workaround for ddivs to run under PCI-E */
		if (pci_allow_pseudo_children)
			return (DDI_SUCCESS);

		/*
		 * The child was not merged into a h/w node,
		 * but there's not much we can do with it other
		 * than return failure to cause the node to be removed.
		 */
		cmn_err(CE_WARN, "!%s@%s: %s.conf properties not merged",
		    ddi_driver_name(child), ddi_get_name_addr(child),
		    ddi_driver_name(child));
		ddi_set_name_addr(child, NULL);
		return (DDI_NOT_WELL_FORMED);
	}

	if (ddi_getprop(DDI_DEV_T_NONE, child, DDI_PROP_DONTPASS, "interrupts",
	    -1) != -1) {
		pdptr = kmem_zalloc((sizeof (struct ddi_parent_private_data) +
		    sizeof (struct intrspec)), KM_SLEEP);
		pdptr->par_intr = (struct intrspec *)(pdptr + 1);
		pdptr->par_nintr = 1;
		ddi_set_parent_data(child, pdptr);
	} else
		ddi_set_parent_data(child, NULL);

	if (pcie_error_init(child) != DDI_SUCCESS)
		return (DDI_FAILURE);

	return (DDI_SUCCESS);
}

static void
pepb_uninitchild(dev_info_t *dip)
{
	struct ddi_parent_private_data *pdptr;

	/*
	 * Do it way early.
	 * Otherwise ddi_map() call form pcie_error_fini crashes
	 */
	pcie_error_fini(dip);

	if ((pdptr = ddi_get_parent_data(dip)) != NULL) {
		kmem_free(pdptr, (sizeof (*pdptr) + sizeof (struct intrspec)));
		ddi_set_parent_data(dip, NULL);
	}
	ddi_set_name_addr(dip, NULL);

	/*
	 * Strip the node to properly convert it back to prototype form
	 */
	ddi_remove_minor_node(dip, NULL);

	ddi_prop_remove_all(dip);
}

/*
 * pepb_save_config_regs
 *
 * This routine saves the state of the configuration registers of all
 * the child nodes of each PBM.
 *
 * used by: pepb_detach() on suspends
 *
 * return value: none
 *
 * XXX: Need to save PCI-E config registers including MSI
 */
static void
pepb_save_config_regs(pepb_devstate_t *pepb_p)
{
	int i;
	dev_info_t *dip;
	ddi_acc_handle_t config_handle;

	for (i = 0, dip = ddi_get_child(pepb_p->dip); dip != NULL;
	    i++, dip = ddi_get_next_sibling(dip)) {

		if (pci_config_setup(dip, &config_handle) != DDI_SUCCESS) {
			cmn_err(CE_WARN, "%s%d: can't config space for %s%d\n",
			    ddi_driver_name(pepb_p->dip),
			    ddi_get_instance(pepb_p->dip),
			    ddi_driver_name(dip),
			    ddi_get_instance(dip));
			continue;
		}

		pepb_p->config_state[i].dip = dip;
		pepb_p->config_state[i].command =
		    pci_config_get16(config_handle, PCI_CONF_COMM);
		pepb_p->config_state[i].header_type =
		    pci_config_get8(config_handle, PCI_CONF_HEADER);

		if ((pepb_p->config_state[i].header_type & PCI_HEADER_TYPE_M) ==
		    PCI_HEADER_ONE)
			pepb_p->config_state[i].bridge_control =
			    pci_config_get16(config_handle, PCI_BCNF_BCNTRL);

		pepb_p->config_state[i].cache_line_size =
		    pci_config_get8(config_handle, PCI_CONF_CACHE_LINESZ);
		pepb_p->config_state[i].latency_timer =
		    pci_config_get8(config_handle, PCI_CONF_LATENCY_TIMER);

		if ((pepb_p->config_state[i].header_type &
		    PCI_HEADER_TYPE_M) == PCI_HEADER_ONE)
			pepb_p->config_state[i].sec_latency_timer =
			    pci_config_get8(config_handle,
			    PCI_BCNF_LATENCY_TIMER);

		pci_config_teardown(&config_handle);
	}
	pepb_p->config_state_index = i;
}


/*
 * pepb_restore_config_regs
 *
 * This routine restores the state of the configuration registers of all
 * the child nodes of each PBM.
 *
 * used by: pepb_attach() on resume
 *
 * return value: none
 *
 * XXX: Need to restore PCI-E config registers including MSI
 */
static void
pepb_restore_config_regs(pepb_devstate_t *pepb_p)
{
	int i;
	dev_info_t *dip;
	ddi_acc_handle_t config_handle;

	for (i = 0; i < pepb_p->config_state_index; i++) {
		dip = pepb_p->config_state[i].dip;
		if (pci_config_setup(dip, &config_handle) != DDI_SUCCESS) {
			cmn_err(CE_WARN, "%s%d: can't config space for %s%d\n",
			    ddi_driver_name(pepb_p->dip),
			    ddi_get_instance(pepb_p->dip),
			    ddi_driver_name(dip),
			    ddi_get_instance(dip));
			continue;
		}
		pci_config_put16(config_handle, PCI_CONF_COMM,
		    pepb_p->config_state[i].command);
		if ((pepb_p->config_state[i].header_type & PCI_HEADER_TYPE_M) ==
		    PCI_HEADER_ONE)
			pci_config_put16(config_handle, PCI_BCNF_BCNTRL,
			    pepb_p->config_state[i].bridge_control);

		pci_config_put8(config_handle, PCI_CONF_CACHE_LINESZ,
		    pepb_p->config_state[i].cache_line_size);
		pci_config_put8(config_handle, PCI_CONF_LATENCY_TIMER,
		    pepb_p->config_state[i].latency_timer);

		if ((pepb_p->config_state[i].header_type &
		    PCI_HEADER_TYPE_M) == PCI_HEADER_ONE)
			pci_config_put8(config_handle, PCI_BCNF_LATENCY_TIMER,
			    pepb_p->config_state[i].sec_latency_timer);

		pci_config_teardown(&config_handle);
	}
}

static int
pepb_pcie_device_type(dev_info_t *dip, int *port_type)
{
	ddi_acc_handle_t handle;

	if (pci_config_setup(dip, &handle) != DDI_SUCCESS)
		return (DDI_FAILURE);
	*port_type = pepb_pcie_port_type(dip, handle);
	pci_config_teardown(&handle);

	/* No PCIe CAP regs, we are not PCIe device_type */
	if (*port_type < 0)
		return (DDI_FAILURE);

	/* check for all PCIe device_types */
	if ((*port_type == PCIE_PCIECAP_DEV_TYPE_UP) ||
	    (*port_type == PCIE_PCIECAP_DEV_TYPE_DOWN) ||
	    (*port_type == PCIE_PCIECAP_DEV_TYPE_ROOT) ||
	    (*port_type == PCIE_PCIECAP_DEV_TYPE_PCI2PCIE))
		return (DDI_SUCCESS);

	return (DDI_FAILURE);

}

/*
 * This function initializes internally generated interrupts only.
 * It does not affect any interrupts generated by downstream devices
 * or the forwarding of them.
 *
 * Enable Device Specific Interrupts or Hotplug features here.
 * Enabling features may change how many interrupts are requested
 * by the device.  If features are not enabled first, the
 * device might not ask for any interrupts.
 */
static int
pepb_intr_init(pepb_devstate_t *pepb_p, int intr_type)
{
	dev_info_t	*dip = pepb_p->dip;
	int		request = 1, count, x;
	int		ret;
	int		intr_cap = 0;
	int		inum = 0;
	ddi_intr_handler_t	**isr_tab = NULL;
	int		isr_tab_size = 0;

	PEPB_DEBUG((CE_NOTE, "pepb_intr_init: Attaching %s handler\n",
	    (intr_type == DDI_INTR_TYPE_MSI) ? "MSI" : "INTx"));

	/*
	 * Get number of requested interrupts.	If none requested or DDI_FAILURE
	 * just return DDI_SUCCESS.
	 *
	 * Several Bridges/Switches will not have this property set, resulting
	 * in a FAILURE, if the device is not configured in a way that
	 * interrupts are needed. (eg. hotplugging)
	 */
	ret = ddi_intr_get_nintrs(dip, intr_type, &request);
	if ((ret != DDI_SUCCESS) || (request == 0)) {
		PEPB_DEBUG((CE_NOTE, "ddi_intr_get_nintrs ret:%d req:%d\n",
		    ret, request));
		return (DDI_FAILURE);
	}

	PEPB_DEBUG((CE_NOTE, "ddi_intr_get_nintrs: NINTRS = %x\n", request));

	/* Allocate an array of interrupt handlers */
	pepb_p->htable_size = sizeof (ddi_intr_handle_t) * request;
	pepb_p->htable = kmem_zalloc(pepb_p->htable_size, KM_SLEEP);
	pepb_p->soft_state |= PEPB_SOFT_STATE_INIT_HTABLE;

	ret = ddi_intr_alloc(dip, pepb_p->htable, intr_type, inum, request,
	    &count, DDI_INTR_ALLOC_NORMAL);
	if ((ret != DDI_SUCCESS) || (count == 0)) {
		PEPB_DEBUG((CE_NOTE, "ddi_intr_alloc() ret: %d ask: %d"
		    " actual: %d\n", ret, request, count));
		goto fail;
	}

	/* Save the actual number of interrupts allocated */
	pepb_p->intr_count = count;
#ifdef	DEBUG
	if (count < request)
		PEPB_DEBUG((CE_WARN, "Requested Intr: %d Received: %d\n",
		    request, count));
#endif	/* DEBUG */
	pepb_p->soft_state |= PEPB_SOFT_STATE_INIT_ALLOC;

	/* Get interrupt priority */
	ret = ddi_intr_get_pri(pepb_p->htable[0], &pepb_p->intr_priority);
	if (ret != DDI_SUCCESS) {
		PEPB_DEBUG((CE_WARN, "ddi_intr_get_pri() ret: %d\n", ret));
		goto fail;
	}

	/* initialize the interrupt mutex */
	mutex_init(&pepb_p->pepb_mutex, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(pepb_p->intr_priority));
	pepb_p->soft_state |= PEPB_SOFT_STATE_INIT_MUTEX;

	isr_tab_size = sizeof (*isr_tab) * pepb_p->intr_count;
	isr_tab = kmem_alloc(isr_tab_size, KM_SLEEP);
	if (pepb_enable_msi && pepb_p->intr_count == 2 &&
	    intr_type == DDI_INTR_TYPE_MSI &&
	    pepb_is_nvidia_root_port(dip) == DDI_SUCCESS) {
		isr_tab[0] = pepb_pwr_msi_intr;
		isr_tab[1] = pepb_err_msi_intr;
	} else
		isr_tab[0] = pepb_intx_intr;

	for (count = 0; count < pepb_p->intr_count; count++) {
		ret = ddi_intr_add_handler(pepb_p->htable[count],
		    isr_tab[count], (caddr_t)pepb_p,
		    (caddr_t)(uintptr_t)(inum + count));

		if (ret != DDI_SUCCESS) {
			PEPB_DEBUG((CE_WARN, "Cannot add interrupt(%d)\n",
			    ret));
			break;
		}
	}

	kmem_free(isr_tab, isr_tab_size);

	/* If unsucessful, remove the added handlers */
	if (ret != DDI_SUCCESS) {
		for (x = 0; x < count; x++) {
			(void) ddi_intr_remove_handler(pepb_p->htable[x]);
		}
		goto fail;
	}

	pepb_p->soft_state |= PEPB_SOFT_STATE_INIT_HANDLER;

	(void) ddi_intr_get_cap(pepb_p->htable[0], &intr_cap);

	if (intr_cap & DDI_INTR_FLAG_BLOCK) {
		(void) ddi_intr_block_enable(pepb_p->htable,
		    pepb_p->intr_count);
		pepb_p->soft_state |= PEPB_SOFT_STATE_INIT_BLOCK;
	} else {
		for (count = 0; count < pepb_p->intr_count; count++) {
			(void) ddi_intr_enable(pepb_p->htable[count]);
		}
	}
	pepb_p->soft_state |= PEPB_SOFT_STATE_INIT_ENABLE;

	/* Save the interrupt type */
	pepb_p->intr_type = intr_type;

	return (DDI_SUCCESS);

fail:
	pepb_intr_fini(pepb_p);

	return (DDI_FAILURE);
}

static void
pepb_intr_fini(pepb_devstate_t *pepb_p)
{
	int x;
	int count = pepb_p->intr_count;
	int flags = pepb_p->soft_state;

	if ((flags & PEPB_SOFT_STATE_INIT_ENABLE) &&
	    (flags & PEPB_SOFT_STATE_INIT_BLOCK)) {
		(void) ddi_intr_block_disable(pepb_p->htable, count);
		flags &= ~(PEPB_SOFT_STATE_INIT_ENABLE |
		    PEPB_SOFT_STATE_INIT_BLOCK);
	}

	if (flags & PEPB_SOFT_STATE_INIT_MUTEX) {
		/* destroy the mutex */
		mutex_destroy(&pepb_p->pepb_mutex);
	}

	for (x = 0; x < count; x++) {
		if (flags & PEPB_SOFT_STATE_INIT_ENABLE)
			(void) ddi_intr_disable(pepb_p->htable[x]);

		if (flags & PEPB_SOFT_STATE_INIT_HANDLER)
			(void) ddi_intr_remove_handler(pepb_p->htable[x]);

		if (flags & PEPB_SOFT_STATE_INIT_ALLOC)
			(void) ddi_intr_free(pepb_p->htable[x]);
	}

	flags &= ~(PEPB_SOFT_STATE_INIT_ENABLE |
	    PEPB_SOFT_STATE_INIT_HANDLER |
	    PEPB_SOFT_STATE_INIT_ALLOC | PEPB_SOFT_STATE_INIT_MUTEX);

	if (flags & PEPB_SOFT_STATE_INIT_HTABLE)
		kmem_free(pepb_p->htable, pepb_p->htable_size);

	flags &= ~PEPB_SOFT_STATE_INIT_HTABLE;

	pepb_p->soft_state &= flags;
}

/*
 * pepb_intx_intr()
 *
 * This is the common interrupt handler for both hotplug and non-hotplug
 * interrupts. For handling hot plug interrupts it calls pciehpc_intr().
 *
 * NOTE: Currently only hot plug interrupts are enabled so it simply
 * calls pciehpc_intr(). This is for INTx interrupts *ONLY*.
 */
/*ARGSUSED*/
static uint_t
pepb_intx_intr(caddr_t arg, caddr_t arg2)
{
	pepb_devstate_t *pepb_p = (pepb_devstate_t *)arg;
	int ret = DDI_INTR_UNCLAIMED;

	if (!(pepb_p->soft_state & PEPB_SOFT_STATE_INIT_ENABLE))
		return (DDI_INTR_UNCLAIMED);

	mutex_enter(&pepb_p->pepb_mutex);

	/* if HPC is initialized then call the interrupt handler */
	if (pepb_p->inband_hpc == INBAND_HPC_PCIE)
		ret =  pciehpc_intr(pepb_p->dip);

	mutex_exit(&pepb_p->pepb_mutex);

	return (ret);
}

/*
 * pepb_is_nvidia_root_port()
 *
 * This helper function checks if the device is a Nvidia RC or not
 */
static int
pepb_is_nvidia_root_port(dev_info_t *dip)
{
	int ret = DDI_FAILURE;
	ddi_acc_handle_t handle;

	if (pci_config_setup(dip, &handle) != DDI_SUCCESS)
		return (ret);

	if ((pci_config_get16(handle, PCI_CONF_VENID) == NVIDIA_VENDOR_ID) &&
	    NVIDIA_PCIE_RC_DEV_ID(pci_config_get16(handle, PCI_CONF_DEVID)))
		ret = DDI_SUCCESS;

	pci_config_teardown(&handle);
	return (ret);
}

/*
 * pepb_pwr_msi_intr()
 *
 * This is the MSI interrupt handler for PM related events.
 */
/*ARGSUSED*/
static uint_t
pepb_pwr_msi_intr(caddr_t arg, caddr_t arg2)
{
	pepb_devstate_t	*pepb_p = (pepb_devstate_t *)arg;

	if (!(pepb_p->soft_state & PEPB_SOFT_STATE_INIT_ENABLE))
		return (DDI_INTR_UNCLAIMED);

	mutex_enter(&pepb_p->pepb_mutex);
	PEPB_DEBUG((CE_NOTE, "pepb_pwr_msi_intr: received intr number %d\n",
	    (int)(uintptr_t)arg2));
	mutex_exit(&pepb_p->pepb_mutex);
	return (DDI_INTR_CLAIMED);
}

static int
pepb_pcie_port_type(dev_info_t *dip, ddi_acc_handle_t handle)
{
	uint_t cap_loc;

	/* Need to look at the port type information here */
	cap_loc = ddi_prop_get_int(DDI_DEV_T_ANY, dip,
	    DDI_PROP_DONTPASS, "pcie-capid-pointer", PCI_CAP_NEXT_PTR_NULL);

	return (cap_loc == PCI_CAP_NEXT_PTR_NULL ? -1 :
	    pci_config_get16(handle, cap_loc + PCIE_PCIECAP) &
		PCIE_PCIECAP_DEV_TYPE_MASK);
}

/*ARGSUSED*/
int
pepb_fm_init(dev_info_t *dip, dev_info_t *tdip, int cap,
    ddi_iblock_cookie_t *ibc)
{
	pepb_devstate_t	 *pepb = ddi_get_soft_state(pepb_state,
	    ddi_get_instance(dip));

	ASSERT(ibc != NULL);
	*ibc = pepb->pepb_fm_ibc;

	return (pepb->pepb_fmcap);
}

/*ARGSUSED*/
int
pepb_fm_callback(dev_info_t *dip, ddi_fm_error_t *derr, const void *no_used)
{
	pepb_devstate_t *pepb_p = (pepb_devstate_t *)
	    ddi_get_soft_state(pepb_state, ddi_get_instance(dip));

	mutex_enter(&pepb_p->pepb_err_mutex);
	pci_ereport_post(dip, derr, NULL);
	mutex_exit(&pepb_p->pepb_err_mutex);
	return (derr->fme_status);
}

/*ARGSUSED*/
static uint_t
pepb_err_msi_intr(caddr_t arg, caddr_t arg2)
{
	pepb_devstate_t *pepb_p = (pepb_devstate_t *)arg;
	ddi_fm_error_t derr;

	bzero(&derr, sizeof (ddi_fm_error_t));

	if (!(pepb_p->soft_state & PEPB_SOFT_STATE_INIT_ENABLE))
		return (DDI_INTR_UNCLAIMED);

	mutex_enter(&pepb_p->pepb_peek_poke_mutex);
	mutex_enter(&pepb_p->pepb_err_mutex);
	PEPB_DEBUG((CE_NOTE, "pepb_err_msi_intr: received intr number %d\n",
	    (int)(uintptr_t)arg2));

	/* if HPC is initialized then call the interrupt handler */
	if (pepb_p->pepb_fmcap & DDI_FM_EREPORT_CAPABLE)
		pci_ereport_post(pepb_p->dip, &derr, NULL);

	if ((pepb_panic_fatal && derr.fme_status == DDI_FM_FATAL) ||
	    (pepb_panic_unknown && derr.fme_status == DDI_FM_UNKNOWN))
		fm_panic("%s-%d: PCI(-X) Express Fatal Error",
		    ddi_driver_name(pepb_p->dip),
		    ddi_get_instance(pepb_p->dip));

	mutex_exit(&pepb_p->pepb_err_mutex);
	mutex_exit(&pepb_p->pepb_peek_poke_mutex);

	return (DDI_INTR_CLAIMED);
}

static int
pepb_check_slot_disabled(dev_info_t *dip)
{
	int			rval = 0;
	uint8_t			pcie_cap_ptr;
	ddi_acc_handle_t	config_handle;

	if (pci_config_setup(dip, &config_handle) != DDI_SUCCESS)
		return (rval);

	pcie_cap_ptr = ddi_prop_get_int(DDI_DEV_T_ANY, dip,
	    DDI_PROP_DONTPASS, "pcie-capid-pointer", PCI_CAP_NEXT_PTR_NULL);

	if (pcie_cap_ptr != PCI_CAP_NEXT_PTR_NULL) {
		if (pci_config_get16(config_handle,
		    pcie_cap_ptr + PCIE_LINKCTL) & PCIE_LINKCTL_LINK_DISABLE)
			rval = 1;
	}

	pci_config_teardown(&config_handle);
	return (rval);
}

static int
pepb_open(dev_t *devp, int flags, int otyp, cred_t *credp)
{
	return ((pcihp_get_cb_ops())->cb_open(devp, flags, otyp, credp));
}

static int
pepb_close(dev_t dev, int flags, int otyp, cred_t *credp)
{
	return ((pcihp_get_cb_ops())->cb_close(dev, flags, otyp, credp));
}

static int
pepb_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
	int *rvalp)
{
	return ((pcihp_get_cb_ops())->cb_ioctl(dev, cmd, arg, mode, credp,
	    rvalp));
}

static int
pepb_prop_op(dev_t dev, dev_info_t *dip, ddi_prop_op_t prop_op,
	int flags, char *name, caddr_t valuep, int *lengthp)
{
	return ((pcihp_get_cb_ops())->cb_prop_op(dev, dip, prop_op, flags,
	    name, valuep, lengthp));
}

static int
pepb_info(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
	return (pcihp_info(dip, cmd, arg, result));
}
