/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)wrsmplat.c	1.12	05/10/19 SMI"

/*
 * Serengeti WRSM platform-specific module
 */


#include <sys/types.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/cmn_err.h>
#include <sys/modctl.h>
#include <sys/kmem.h>
#include <sys/disp.h>
#include <sys/int_fmtio.h>
#include <sys/cpuvar.h>
#include <sys/cpu_module.h>

#include <sys/wrsm_config.h>
#include <sys/wrsm_plat_impl.h>
#include <sys/wci_regs.h>
#include <sys/wci_offsets.h>
#include <sys/wci_common.h>
#include <sys/sgsbbc.h>
#include <sys/sgsbbc_mailbox.h>

#ifdef DEBUG
#define	WP_DEBUG 1
#define	WP_QUEUE 2
#define	WP_LINK 4
#define	WP_THREAD 8
#define	WP_MBOX 16

static uint_t wrsm_plat_debug = 0;

#define	DPRINTF(a, b) { if (wrsm_plat_debug & a) cmn_err b; }
#else
#define	DPRINTF(a, b) { }
#endif

static wrsm_node_types_t node_type = wrsm_node_serengeti;

/* this variable allows for runtime changes of the use of the mailbox */
boolean_t wrsm_use_sgsbbc = B_TRUE;

static uint64_t read_reg(volatile uchar_t *regs, uint64_t offset);
static void write_reg(volatile uchar_t *regs, uint64_t offset, uint64_t value);

/* Converts a 10-bit safari port id to its 4-bit SSM node id */
#define	PORT2NODE(p)	(((p) >> 5) & 0xf)

/*
 * Module linkage information for the kernel.
 */
static struct modlmisc modlmisc = {
	&mod_miscops,
	"Sun Fire wrsm platmod 1.12"
};

static struct modlinkage modlinkage = {
	MODREV_1, (void *)&modlmisc, NULL
};


typedef struct wrsmplat_mbox_msg {
	uint32_t msg_type;
	boolean_t loopback;
	void *msg_data;
	size_t msg_size;
	struct wrsmplat_mbox_msg *next;
} wrsmplat_mbox_msg_t;

/*
 * If stop_thread is set for a particular link of a wci, that means that
 * the thread trying to bringup that link should give up and exit.
 */
typedef struct wrsmplat_link {
	kmutex_t mutex;
	boolean_t thread_active;
	boolean_t stop_thread;
	kcondvar_t cv;
} wrsmplat_link_t;

typedef struct wrsmplat_wci {
	safari_port_t wci_id;
	boolean_t valid;
	boolean_t suspended;
	wrsmplat_link_t link[WRSM_MAX_LINKS_PER_WCI];
} wrsmplat_wci_t;

static struct wrsmplat_state {
	boolean_t thread_stop;
	wrsmplat_mbox_msg_t *msg_queue;
	kmutex_t queue_lock;
	wrsm_plat_ops_t *cb_ops;
	kcondvar_t msg_cv;
	kmutex_t cv_lock;
	uint_t thread_count;

	sbbc_msg_t junk_message;	/* for asynchronous mailbox */
	wrsm_linkisup_msg_t junk_stuff;	/* buffer for incoming message */
	kmutex_t intr_lock;
	uint_t intr_state;
	wrsmplat_wci_t wci[WRSM_MAX_WCIS];
	kmutex_t thread_count_lock;
	kmutex_t tnid_map_lock;
} wrsmplat_state = {0};

/*
 * serengeti implementations of these functions are empty since
 * for serengeti cesr ids can be specified in 6 bits given the
 * maximum of 48 cores
 */

void
wrsmplat_set_asi_cesr_id(void)
{
}

void
wrsmplat_clr_asi_cesr_id(void)
{
}

/* support when SC is not available */
static void
stop_thread(safari_port_t wci_id, int link)
{
	int i;
	wrsmplat_link_t *l;

	for (i = 0; i < WRSM_MAX_WCIS; i++) {
		if (wrsmplat_state.wci[i].valid &&
		    wrsmplat_state.wci[i].wci_id == wci_id) {
			l = &wrsmplat_state.wci[i].link[link];
			mutex_enter(&l->mutex);
			while (l->thread_active) {
				DPRINTF(WP_THREAD, (CE_CONT,
				    "stop_thread: wci %d link %d "
				    "waiting for thread to stop",
				    wci_id, link));
				l->stop_thread = B_TRUE;
				cv_wait(&l->cv, &l->mutex);
			}
			mutex_exit(&l->mutex);
			DPRINTF(WP_THREAD, (CE_CONT, "stop_thread: "
			    "wci %d link %d thread stopped",
			    wci_id, link));
			break;
		}
	}
}

static void
start_thread(safari_port_t wci_id, int link, void (*proc)(), caddr_t arg)
{
	int i;
	boolean_t found = B_FALSE;
	wrsmplat_link_t *l;

	/* First, look to see if WCI already exists */
	for (i = 0; i < WRSM_MAX_WCIS; i++) {
		if (wrsmplat_state.wci[i].valid &&
		    wrsmplat_state.wci[i].wci_id == wci_id) {
			/* Don't start the thread if wci was suspended */
			if (wrsmplat_state.wci[i].suspended) {
				DPRINTF(WP_THREAD, (CE_CONT, "wci %d is "
				    "suspended, can't start thread", wci_id));
				return;
			}
			l = &wrsmplat_state.wci[i].link[link];
			mutex_enter(&l->mutex);
			if (l->thread_active) {
				mutex_exit(&l->mutex);
				DPRINTF(WP_THREAD, (CE_CONT, "wci %d link %d "
				    "thread already running", wci_id, link));
				return;
			}
			l->thread_active = B_TRUE;
			l->stop_thread = B_FALSE;
			mutex_exit(&l->mutex);
			found = B_TRUE;
			DPRINTF(WP_THREAD, (CE_CONT, "wci %d exists", wci_id));
			break;
		}
	}

	/* If not, then search for free space and add wci to the list */
	if (!found) {
		for (i = 0; i < WRSM_MAX_WCIS; i++) {
			if (!wrsmplat_state.wci[i].valid) {
				wrsmplat_state.wci[i].wci_id = wci_id;
				l = &wrsmplat_state.wci[i].link[link];
				mutex_init(&l->mutex, NULL, MUTEX_DRIVER,
				    NULL);
				cv_init(&l->cv, NULL, CV_DEFAULT, NULL);
				l->thread_active = B_TRUE;
				l->stop_thread = B_FALSE;
				wrsmplat_state.wci[i].valid = B_TRUE;
				wrsmplat_state.wci[i].suspended = B_FALSE;
				found = B_TRUE;
				DPRINTF(WP_THREAD, (CE_CONT, "created wci %d",
				    wci_id));
				break;
			}
		}
	}

	if (found || wci_id == -1) {
		mutex_enter(&wrsmplat_state.thread_count_lock);
		++wrsmplat_state.thread_count;
		DPRINTF(WP_THREAD, (CE_CONT,
		    "start_thread: wci %d, link %d, thread #%d",
		    wci_id, link, wrsmplat_state.thread_count));
		mutex_exit(&wrsmplat_state.thread_count_lock);
		(void) thread_create(NULL, 0, proc, arg,
		    0, &p0, TS_RUN, minclsyspri);
	/* LINTED - E_NOP_ELSE_STMT */
	} else {
		DPRINTF(WP_DEBUG, (CE_WARN, "start_thread: bad wci/link: "
		    "wci %d link %d", wci_id, link));
	}
}

static void
exit_thread(safari_port_t wci_id, int link)
{
	int i;
	wrsmplat_link_t *l;

	for (i = 0; i < WRSM_MAX_WCIS; i++) {
		if (wrsmplat_state.wci[i].valid &&
		    wrsmplat_state.wci[i].wci_id == wci_id) {
			l = &wrsmplat_state.wci[i].link[link];
			mutex_enter(&l->mutex);
			l->thread_active = B_FALSE;
			cv_broadcast(&l->cv);
			mutex_exit(&l->mutex);
		}
	}
	mutex_enter(&wrsmplat_state.thread_count_lock);
	--wrsmplat_state.thread_count;
	DPRINTF(WP_THREAD, (CE_CONT, "exit_thread: thread "
	    "exiting for wci %d link %d. There are %d threads left",
	    wci_id, link, wrsmplat_state.thread_count));
	mutex_exit(&wrsmplat_state.thread_count_lock);

	thread_exit();
}

static boolean_t
check_stop_thread(safari_port_t wci_id, int link)
{
	int i;

	if (wrsmplat_state.thread_stop) {
		return (B_TRUE);
	}
	for (i = 0; i < WRSM_MAX_WCIS; i++) {
		if (wrsmplat_state.wci[i].valid &&
		    wrsmplat_state.wci[i].wci_id == wci_id) {
			return (wrsmplat_state.wci[i].link[link].stop_thread);
		}
	}
	return (B_FALSE);
}

/* Use default timeout */
#define	SC_TIMEOUT_SEC MBOX_DEFAULT_TIMEOUT

static uint_t wrsmplat_handler(caddr_t arg);

static void queue_message(uint32_t msg_type, boolean_t loopback,
    void *msg_data, size_t msg_size);
static wrsmplat_mbox_msg_t *dequeue_msg(void);
static void process_message(void);
static void wrsmplat_thread(void *arg);

/* used to start a thread */
typedef struct wrsmplat_thread_data {
	wrsm_uplink_msg_t *msg;
	volatile uchar_t *wrsm_regs;
	boolean_t loopback;
	fmnodeid_t remote_fmnodeid;
	gnid_t remote_gnid;
} wrsmplat_thread_data_t;

static void activate_link(wrsmplat_thread_data_t *data);
static void deactivate_link(safari_port_t portid,
    volatile uchar_t *wrsm_regs, linkid_t link_num);
static int try_activate(wrsm_uplink_msg_t *msg, volatile uchar_t *wrsm_regs);

int
_init(void)
{
	int rc = 0;
	DPRINTF(WP_DEBUG, (CE_CONT, "serengeti:wrsmplat _init"));

	wrsmplat_state.msg_queue = NULL;
	mutex_init(&wrsmplat_state.queue_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&wrsmplat_state.cv_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&wrsmplat_state.intr_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&wrsmplat_state.msg_cv, NULL, CV_DRIVER, NULL);

	if (!wrsm_use_sgsbbc) {
		cmn_err(CE_WARN, "wrsm in test mode, not using SC\n");
	}

	wrsmplat_state.intr_state = SBBC_INTR_IDLE;
	wrsmplat_state.junk_message.msg_type.type = WILDCAT_RSM_MBOX;
	wrsmplat_state.junk_message.msg_buf =
	    (caddr_t)&wrsmplat_state.junk_stuff;
	wrsmplat_state.junk_message.msg_len =
	    sizeof (wrsmplat_state.junk_stuff);
	if ((rc = sbbc_mbox_reg_intr(WILDCAT_RSM_MBOX,
	    &wrsmplat_handler, &wrsmplat_state.junk_message,
	    &wrsmplat_state.intr_state, &wrsmplat_state.intr_lock))
	    != 0) {
		cmn_err(CE_WARN, "sbbc_mbox_reg_intr failed %d", rc);
		mutex_destroy(&wrsmplat_state.queue_lock);
		mutex_destroy(&wrsmplat_state.cv_lock);
		mutex_destroy(&wrsmplat_state.intr_lock);
		cv_destroy(&wrsmplat_state.msg_cv);
		return (rc);
	}
	DPRINTF(WP_DEBUG, (CE_CONT, "sbbc_mbox_reg_intr SUCCESS!"));
	mutex_init(&wrsmplat_state.thread_count_lock, NULL,
	    MUTEX_DRIVER, NULL);
	mutex_init(&wrsmplat_state.tnid_map_lock, NULL,
	    MUTEX_DRIVER, NULL);

	if ((rc = mod_install(&modlinkage)) != 0) {
		sbbc_mbox_unreg_intr(WILDCAT_RSM_MBOX, &wrsmplat_handler);
		mutex_destroy(&wrsmplat_state.queue_lock);
		mutex_destroy(&wrsmplat_state.cv_lock);
		mutex_destroy(&wrsmplat_state.intr_lock);
		cv_destroy(&wrsmplat_state.msg_cv);
		mutex_destroy(&wrsmplat_state.thread_count_lock);
		mutex_destroy(&wrsmplat_state.tnid_map_lock);
		return (rc);
	}
	return (0);
}

int
_fini(void)
{
	int rc;

	DPRINTF(WP_DEBUG, (CE_CONT, "serengeti:wrsmplat _fini"));

	if ((rc = mod_remove(&modlinkage)) != 0) {
		return (rc);
	}
	/*
	 * This blocks waiting for all the child threads to die
	 */
	(void) wrsmplat_unreg_callbacks();

	mutex_destroy(&wrsmplat_state.queue_lock);
	mutex_destroy(&wrsmplat_state.cv_lock);
	cv_destroy(&wrsmplat_state.msg_cv);

	mutex_destroy(&wrsmplat_state.intr_lock);
	sbbc_mbox_unreg_intr(WILDCAT_RSM_MBOX, &wrsmplat_handler);

	mutex_destroy(&wrsmplat_state.thread_count_lock);
	mutex_destroy(&wrsmplat_state.tnid_map_lock);

	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}


/*
 * Handle asynchronous messages from the SC up to the kernel
 */
/* ARGSUSED */
uint_t
wrsmplat_handler(caddr_t arg)
{
	wrsm_linkisup_msg_t *linkisup_msg;

	mutex_enter(&wrsmplat_state.intr_lock);
	wrsmplat_state.intr_state = SBBC_INTR_RUNNING;
	mutex_exit(&wrsmplat_state.intr_lock);


	DPRINTF(WP_MBOX, (CE_CONT, "wrsmplat_handler: received message,"
	    " type=%d, sub-type: %d  status = %d, length= %d, data=%x %x",
	    wrsmplat_state.junk_message.msg_type.type,
	    wrsmplat_state.junk_message.msg_type.sub_type,
	    wrsmplat_state.junk_message.msg_status,
	    wrsmplat_state.junk_message.msg_bytes,
	    wrsmplat_state.junk_message.msg_data[0],
	    wrsmplat_state.junk_message.msg_data[1]));


	if (wrsmplat_state.junk_message.msg_type.type != WILDCAT_RSM_MBOX) {
		cmn_err(CE_WARN, "wrsmplat_handler: wrong message type");
	} else {
		linkisup_msg = (wrsm_linkisup_msg_t *)
		    wrsmplat_state.junk_message.msg_buf;

		switch (linkisup_msg->async_msg_type) {

		case LINKISUP:
			DPRINTF(WP_MBOX, (CE_CONT, "_handler: LINKISUP "
			    "wci %d link %d",
			    linkisup_msg->link_info.wci_port_id,
			    linkisup_msg->link_info.link_num));

			/* put this message on a queue to be handled later */
			queue_message(LINKISUP, B_FALSE,
			    &linkisup_msg->link_info,
			    sizeof (linkisup_msg->link_info));

		break;
		default:
			cmn_err(CE_WARN, "wrsmplat_handler:"
			    " unhandled async_msg_type");
		}
	}

	mutex_enter(&wrsmplat_state.intr_lock);
	wrsmplat_state.intr_state = SBBC_INTR_IDLE;
	mutex_exit(&wrsmplat_state.intr_lock);

	return (0);
}

wrsm_node_types_t
wrsmplat_get_node_type(void)
{
	return (node_type);
}


int
wrsmplat_uplink(safari_port_t wci, linkid_t link, gnid_t gnid,
    fmnodeid_t fmnodeid, uint64_t partition_version, uint32_t controller_id,
    boolean_t loopback)
{
	wrsm_uplink_msg_t msg = {0};

	DPRINTF(WP_DEBUG, (CE_CONT, "wrsmplat_uplink wci %d link %d",
	    wci, link));

	msg.config_data.partition_version = partition_version;
	msg.config_data.partition_id = controller_id;
	msg.config_data.fmnodeid = fmnodeid;
	msg.config_data.gnid = gnid;
	msg.wci_port_id = wci;
	msg.link_num = link;

	queue_message(UPLINK, loopback, &msg, sizeof (msg));
	return (0);
}

int
wrsmplat_downlink(safari_port_t wci, linkid_t link, boolean_t loopback)
{
	wrsm_link_msg_t msg = {0};

	DPRINTF(WP_DEBUG, (CE_CONT, "wrsmplat_downlink wci %d link %d",
	    wci, link));
	msg.wci_port_id = wci;
	msg.link_num = link;

	queue_message(DOWNLINK, loopback, &msg, sizeof (msg));
	return (0);
}

int
wrsmplat_linktest(safari_port_t wci, wrsm_linktest_arg_t *linktest)
{
	fmnodeid_t remote_fmnodeid;
	gnid_t remote_gnid;
	linkid_t remote_link;
	safari_port_t remote_port;
	volatile unsigned char *wrsm_regs = NULL;
	wci_sw_link_control_u link_control;
	wci_sw_link_status_u link_status;
	uint64_t offset = linktest->link_num * STRIDE_WCI_SW_LINK_CONTROL;

	if (linktest->link_num >= WRSM_LINKS_PER_WCI)
		return (EINVAL);

	(*wrsmplat_state.cb_ops->get_remote_data)(wci, linktest->link_num,
	    &remote_fmnodeid, &remote_gnid, &remote_link, &remote_port,
	    &wrsm_regs);

	if (!wrsm_regs)
		return (ENXIO);

	link_control.val = read_reg(wrsm_regs,
	    ADDR_WCI_SW_LINK_CONTROL + offset);
	if (link_control.bit.link_state != LINK_STATE_IN_USE)
		return (EAGAIN);

	link_control.val = read_reg(wrsm_regs,
	    ADDR_WCI_SW_LINK_CONTROL + offset);
	linktest->link_control = link_control.val;
	link_control.bit.usr_data_1 = linktest->pattern & 0xffff;
	link_control.bit.usr_data_2 = ((linktest->pattern >> 16) & 0x3);
	write_reg(wrsm_regs, ADDR_WCI_SW_LINK_CONTROL + offset,
	    link_control.val);
	linktest->link_error_count = read_reg(wrsm_regs,
	    ADDR_WCI_SW_LINK_ERROR_COUNT + offset);
	linktest->link_esr = read_reg(wrsm_regs, ADDR_WCI_LINK_ESR);
	linktest->sw_esr = read_reg(wrsm_regs, ADDR_WCI_SW_ESR);
	link_status.val = read_reg(wrsm_regs,
	    ADDR_WCI_SW_LINK_STATUS + offset);
	linktest->link_status = link_status.val;
	linktest->pattern = link_status.bit.farend_ustat_1 |
	    (link_status.bit.farend_ustat_2 << 16);

	return (0);
}

int
wrsmplat_set_led(safari_port_t wci, linkid_t link, int led_state)
{
	wrsm_link_led_msg_t msg = {0};

	DPRINTF(WP_DEBUG, (CE_CONT, "wrsmplat_set_led, mode = %d", led_state));
	msg.wci_port_id = wci;
	msg.link_num = link;
	msg.led_state = led_state;
	queue_message(SETLEDSTATE, B_FALSE, &msg, sizeof (msg));
	return (0);
}

int
wrsmplat_alloc_slices(ncslice_bitmask_t requested, ncslice_bitmask_t *granted)
{

	wrsm_ncslice_claim_msg_t msg = {0};
	wrsm_ncslice_claim_msg_t answer = {0};

	sbbc_msg_t request = {0};
	sbbc_msg_t response = {0};
	int rc;
	int i;
	ncslice_bitmask_t allocated = {0};

	DPRINTF(WP_DEBUG, (CE_CONT, "wrsmplat_alloc_slices"));

	if (wrsm_use_sgsbbc) {
		msg.requested_ncslices = requested;

		request.msg_type.type = WILDCAT_RSM_MBOX;
		request.msg_type.sub_type = NCSLICE;
		request.msg_len = sizeof (msg);
		request.msg_buf = (caddr_t)&msg;

		response.msg_type.type = WILDCAT_RSM_MBOX;
		response.msg_type.sub_type = NCSLICE;
		response.msg_len = sizeof (answer);
		response.msg_buf = (caddr_t)&answer;

		if ((rc = sbbc_mbox_request_response(&request, &response,
		    SC_TIMEOUT_SEC)) != 0) {
			cmn_err(CE_WARN, "sbbc_mbox_request failed rc=%d", rc);
		} else {
			if (response.msg_status) {
				cmn_err(CE_WARN, "error status %d "
				    "from NCSLICE request",
				    response.msg_status);
			}
			*granted = answer.requested_ncslices;
		}
		return (rc);
	} else {
		/*
		 * We are only allowed to use NC slices 161 to 254,
		 * and 0 is reserved.
		 */
		for (i = 161; i < (WRSM_MAX_NCSLICES - 1); ++i)
			if (WRSM_IN_SET(requested, i))
				WRSMSET_ADD(allocated, i);
		WRSMSET_COPY(allocated, *granted);
		return (0);
	}
}

int
wrsmplat_set_seprom(safari_port_t wci_id, uchar_t *seprom_data,
    size_t length)
{
	wrsm_wib_seprom_msg_t msg;

	DPRINTF(WP_DEBUG, (CE_CONT, "wrsmplat_set_seprom"));
	if (length > WIB_SEPROM_MSG_SIZE)
		return (EINVAL);
	msg.wci_port_id = wci_id;
	bcopy(seprom_data, &msg.seprom_data, length);

	queue_message(SETSEPROM, B_FALSE, &msg, sizeof (msg));
	return (0);
}

int
wrsmplat_reg_callbacks(wrsm_plat_ops_t *ops)
{
	DPRINTF(WP_DEBUG, (CE_CONT, "wrsmplat_reg_callbacks"));

	wrsmplat_state.cb_ops = ops;
	wrsmplat_state.thread_stop = B_FALSE;

	if (!wrsm_use_sgsbbc && (wrsmplat_state.thread_count != 0)) {
		DPRINTF(WP_DEBUG, (CE_CONT, "wrsmplat_reg_callbacks: "
		    "called more than once"));
		return (EEXIST);
	}

	/*
	 * no need to lock the thread count:
	 * when using the mailbox : only one thread exists
	 * not using the mailbox  : the wrsmplat thread starts other threads
	 */
	wrsmplat_state.thread_count = 1;
	(void) thread_create(NULL, 0, wrsmplat_thread, NULL, 0, &p0,
	    TS_RUN, minclsyspri);
	return (0);
}

int
wrsmplat_unreg_callbacks(void)
{
	DPRINTF(WP_DEBUG, (CE_CONT, "wrsmplat_unreg_callbacks"));

	wrsmplat_state.thread_stop = B_TRUE;

	/* Wake up the message thread */
	mutex_enter(&wrsmplat_state.cv_lock);
	cv_signal(&wrsmplat_state.msg_cv);
	mutex_exit(&wrsmplat_state.cv_lock);

	/*
	 * Eventually, all of the threads should wake up, notice the
	 * thread_stop flag is set and exit.  When they have all
	 * finished, the count should go to zero and it is safe to
	 * proceed. In case of not using SBBC ther is only one thread.
	 */
	while (wrsmplat_state.thread_count > 0) {
		DPRINTF(WP_THREAD, (CE_CONT, "unreg_callbacks: waiting for "
		    "%d threads to finish", wrsmplat_state.thread_count));
		delay(drv_usectohz(1000 * 1000));
	}
	wrsmplat_state.cb_ops = NULL;
	return (0);
}

static void
queue_message(uint32_t msg_type, boolean_t loopback, void *msg_data,
    size_t msg_size)
{
	wrsmplat_mbox_msg_t *entry, *p;
	DPRINTF(WP_QUEUE, (CE_CONT, "queue_message: msg_type = %d nthreads=%d",
	    msg_type, wrsmplat_state.thread_count));
	entry = kmem_alloc(sizeof (wrsmplat_mbox_msg_t), KM_SLEEP);
	entry->msg_type = msg_type;
	entry->loopback = loopback;
	entry->msg_data = kmem_alloc(msg_size, KM_SLEEP);
	bcopy(msg_data, entry->msg_data, msg_size);
	entry->msg_size = msg_size;
	entry->next = NULL;
	mutex_enter(&wrsmplat_state.queue_lock);
	if (wrsmplat_state.msg_queue == NULL)
		wrsmplat_state.msg_queue = entry;
	else {
		p = wrsmplat_state.msg_queue;
		while (p->next != NULL)
			p = p->next;
		p->next = entry;
	}
	mutex_exit(&wrsmplat_state.queue_lock);

	DPRINTF(WP_QUEUE, (CE_CONT, "queue_message: queued msg_type = %d\n",
	    msg_type));

	mutex_enter(&wrsmplat_state.cv_lock);
	cv_signal(&wrsmplat_state.msg_cv);
	mutex_exit(&wrsmplat_state.cv_lock);

	DPRINTF(WP_QUEUE, (CE_CONT, "queue_message: signaled msg_type = %d\n",
	    msg_type));
}

static wrsmplat_mbox_msg_t *
dequeue_msg(void)
{
	wrsmplat_mbox_msg_t *msg;

	mutex_enter(&wrsmplat_state.queue_lock);
	msg = wrsmplat_state.msg_queue;
	if (msg != NULL)
		wrsmplat_state.msg_queue = msg->next;
	mutex_exit(&wrsmplat_state.queue_lock);

	return (msg);
}

/* ARGSUSED */
static void
wrsmplat_thread(void *arg)
{
	DPRINTF(WP_DEBUG, (CE_CONT, "wrsmplat_thread starting"));

	/* LINTED */
	while (1) {
		process_message();

		/* go to sleep waiting for the next message */
		mutex_enter(&wrsmplat_state.cv_lock);
		cv_wait(&wrsmplat_state.msg_cv, &wrsmplat_state.cv_lock);
		mutex_exit(&wrsmplat_state.cv_lock);

		if (wrsmplat_state.thread_stop) {
			process_message(); /* finish any waiting messages */
			if (!wrsm_use_sgsbbc) { /* there > 1 thread */
				mutex_enter(&wrsmplat_state.thread_count_lock);
			}
			/* in SBBC case there is only one thread */
			--wrsmplat_state.thread_count;
			if (!wrsm_use_sgsbbc) {
				mutex_exit(&wrsmplat_state.thread_count_lock);
			}
			thread_exit();
		}
	}
}

static void
process_message(void)
{
	wrsmplat_mbox_msg_t *entry;
	wrsm_link_msg_t *msg;

	wrsm_link_msg_t question = {0};
	wrsm_uplink_msg_t answer = {0};

	sbbc_msg_t request = {0};
	sbbc_msg_t response = {0};
	int rc = 0;

	boolean_t free_msg = B_TRUE;
	linkid_t remote_link;
	safari_port_t remote_port;
	gnid_t remote_gnid;
	/* LINTED */
	fmnodeid_t remote_fmnodeid;

	while (entry = dequeue_msg()) {
		switch (entry->msg_type) {

		case LINKISUP: {
			/* this is the message from SC, need to get more data */
			msg = (wrsm_link_msg_t *)(entry->msg_data);
				question.wci_port_id = msg->wci_port_id;
				question.link_num = msg->link_num;

				request.msg_len = sizeof (question);
				request.msg_buf = (caddr_t)&question;
				response.msg_len = sizeof (answer);
				response.msg_buf = (caddr_t)&answer;

			request.msg_type.sub_type = LINKDATA;
			request.msg_type.type = WILDCAT_RSM_MBOX;
			response.msg_type.sub_type = LINKDATA;
			response.msg_type.type = WILDCAT_RSM_MBOX;

			if ((rc = sbbc_mbox_request_response(&request,
			    &response, SC_TIMEOUT_SEC)) != 0) {
				cmn_err(CE_WARN, "sbbc_mbox_request_response "
				    "failed rc=%d", rc);
				break;
			}
			if (response.msg_status) {
				cmn_err(CE_WARN, "error status %d "
				    "from LINKDATA request",
				    response.msg_status);
			}

			DPRINTF(WP_MBOX, (CE_CONT, "process_message: wci %d "
			    "link %d fmnid %d gnid %d alink %d port %d "
			    "parver %ld parid %d", msg->wci_port_id,
			    msg->link_num,
			    answer.config_data.fmnodeid,
			    answer.config_data.gnid,
			    answer.link_num,
			    answer.wci_port_id,
			    answer.config_data.partition_version,
			    answer.config_data.partition_id));

			(*wrsmplat_state.cb_ops->link_up)
			    (msg->wci_port_id,
			    (linkid_t)msg->link_num,
			    answer.config_data.fmnodeid,
			    answer.config_data.gnid,
			    (linkid_t)answer.link_num,
			    answer.wci_port_id,
			    answer.config_data.partition_version,
			    answer.config_data.partition_id);

			break;
		}

		case UPLINK: {
			/* this is a message to be sent to SC */
			if (wrsm_use_sgsbbc) {
				request.msg_len = sizeof (wrsm_uplink_msg_t);
				request.msg_buf = (caddr_t)(entry->msg_data);
				response.msg_len = 0;
				response.msg_buf = NULL;

				request.msg_type.sub_type = UPLINK;
				request.msg_type.type = WILDCAT_RSM_MBOX;
				response.msg_type.sub_type = UPLINK;
				response.msg_type.type = WILDCAT_RSM_MBOX;

				if ((rc = sbbc_mbox_request_response(&request,
				    &response, SC_TIMEOUT_SEC)) != 0) {
					cmn_err(CE_WARN,
					    "sbbc_mbox_request_response "
					    "failed rc=%d", rc);
				}
				if (response.msg_status ==
				    SG_MBOX_STATUS_ILLEGAL_PARAMETER) {
					cmn_err(CE_WARN,
					    "SC reports mismatch fmnodeid");
				} else if (response.msg_status) {
					cmn_err(CE_WARN,
					    "unrecognized error status %d "
					    "from UPLINK request",
					    response.msg_status);
				}
			} else {

				wrsm_uplink_msg_t *umsg;
				wrsmplat_thread_data_t *thread_data;

				thread_data = kmem_alloc(
				    sizeof (wrsmplat_thread_data_t), KM_SLEEP);
				umsg = (wrsm_uplink_msg_t *)(entry->msg_data);
				thread_data->msg = umsg;
				thread_data->loopback = entry->loopback;

				(*wrsmplat_state.cb_ops->get_remote_data)
				    (umsg->wci_port_id,
					(linkid_t)umsg->link_num,
					&thread_data->remote_fmnodeid,
					&thread_data->remote_gnid,
					&remote_link, &remote_port,
					&thread_data->wrsm_regs);
				if (!thread_data->wrsm_regs) {
					DPRINTF(WP_DEBUG, (CE_WARN, "mp "
					    "UPLINK: failed to get register "
					    "base"));
					return;
				}
				stop_thread(umsg->wci_port_id,
				    umsg->link_num);
				start_thread(umsg->wci_port_id,
				    umsg->link_num,
				    activate_link,
				    (caddr_t)thread_data);
				free_msg = B_FALSE;
			}
			break;
		}
		case DOWNLINK: {
			/* this is a message to be sent to SC */
			if (wrsm_use_sgsbbc) {
				msg = (wrsm_link_msg_t *)(entry->msg_data);

				request.msg_len = sizeof (wrsm_link_msg_t);
				request.msg_buf = (caddr_t)(entry->msg_data);
				response.msg_len = 0;
				response.msg_buf = NULL;

				request.msg_type.type = WILDCAT_RSM_MBOX;
				request.msg_type.sub_type = DOWNLINK;
				response.msg_type.sub_type = DOWNLINK;
				response.msg_type.type = WILDCAT_RSM_MBOX;

				if ((rc = sbbc_mbox_request_response(&request,
				    &response, SC_TIMEOUT_SEC)) != 0) {
					cmn_err(CE_WARN,
					"sbbc_mbox_request_response failed "
					"rc=%d", rc);
				}
				if (response.msg_status ==
				    SG_MBOX_STATUS_HARDWARE_FAILURE) {
					cmn_err(CE_WARN,
					    "SC reports hardware failure for "
					    "DOWNLINK request");
				} else if (response.msg_status) {
					cmn_err(CE_WARN, "error status %d for "
					    "DOWNLINK request",
					    response.msg_status);
				}

				/* should not call link_down for loopback */
				if ((rc == 0) && (!entry->loopback))
					(*wrsmplat_state.cb_ops->link_down)
					    (msg->wci_port_id,
						msg->link_num);
			} else {

				volatile unsigned char *wrsm_regs = NULL;
				msg = (wrsm_link_msg_t *)(entry->msg_data);

				(*wrsmplat_state.cb_ops->get_remote_data)
				    (msg->wci_port_id,
					(linkid_t)msg->link_num,
					&remote_fmnodeid,
					&remote_gnid,
					&remote_link,
					&remote_port,
					&wrsm_regs);

				stop_thread(msg->wci_port_id,
				    msg->link_num);

				if (wrsm_regs) {
					deactivate_link(msg->wci_port_id,
					    wrsm_regs, msg->link_num);
				/* LINTED - E_NOP_ELSE_STMT */
				} else {
					DPRINTF(WP_DEBUG, (CE_WARN, "mp "
					    "DOWNLINK: failed to get register "
					    "base"));
				}

				/* should not call link_down for loopback */
				if (!entry->loopback)
					(*wrsmplat_state.cb_ops->link_down)
					    (msg->wci_port_id,
						(linkid_t)msg->link_num);
			}
			break;
		}
		case SETLEDSTATE: {
			/* this is a message to be sent to SC */
			if (wrsm_use_sgsbbc) {
				request.msg_len = sizeof (wrsm_link_led_msg_t);
				request.msg_buf = (caddr_t)(entry->msg_data);
				response.msg_len = 0;
				response.msg_buf = NULL;

				request.msg_type.type = WILDCAT_RSM_MBOX;
				request.msg_type.sub_type = SETLEDSTATE;
				response.msg_type.sub_type = SETLEDSTATE;
				response.msg_type.type = WILDCAT_RSM_MBOX;

				if ((rc = sbbc_mbox_request_response(&request,
				    &response, SC_TIMEOUT_SEC)) != 0) {
					cmn_err(CE_WARN,
					"sbbc_mbox_request_response failed "
					"rc=%d", rc);
				}
				if (response.msg_status ==
				    SG_MBOX_STATUS_HARDWARE_FAILURE) {
					cmn_err(CE_WARN,
					    "SC reports hardware failure for "
					    "SETLEDSTATE request");
				} else if (response.msg_status) {
					cmn_err(CE_WARN, "error status %d for "
					    "SETLEDSTATE request",
					    response.msg_status);
				}
			}
			break;
		}
		case SETSEPROM: {
			/* this is a message to be sent to SC */
			if (wrsm_use_sgsbbc) {
				request.msg_len =
					sizeof (wrsm_wib_seprom_msg_t);
				request.msg_buf = (caddr_t)(entry->msg_data);
				response.msg_len = 0;
				response.msg_buf = NULL;

				request.msg_type.type = WILDCAT_RSM_MBOX;
				request.msg_type.sub_type = SETSEPROM;
				response.msg_type.type = WILDCAT_RSM_MBOX;
				response.msg_type.sub_type = SETSEPROM;

				if ((rc = sbbc_mbox_request_response(&request,
				    &response, SC_TIMEOUT_SEC)) != 0) {
					cmn_err(CE_WARN,
					"sbbc_mbox_request_response failed "
					"rc=%d", rc);
				}
				if (response.msg_status ==
				    SG_MBOX_STATUS_HARDWARE_FAILURE) {
					cmn_err(CE_WARN,
					    "SC reports hardware failure for "
					    "SETSEPROM request");
				} else if (response.msg_status) {
					cmn_err(CE_WARN, "error status %d for "
					    "SETSEPROM request",
					    response.msg_status);
				}
			}
			break;
		}
		case NCSLICE: {
			/* NOT HANDLED HERE */
			break;
		}
		default :
			cmn_err(CE_WARN, "process_message: unknown message");
		}
		if (free_msg)
			kmem_free(entry->msg_data, entry->msg_size);
		kmem_free(entry, sizeof (wrsmplat_mbox_msg_t));
	}
}

static uint64_t
read_reg(volatile uchar_t *regs, uint64_t offset)
{
	return (*((uint64_t *)(regs + offset)));
}

static void
write_reg(volatile uchar_t *regs, uint64_t offset, uint64_t value)
{
	*((uint64_t *)(regs + offset)) = value;
}

/* ARGSUSED */
void
wrsmplat_wci_init(volatile uchar_t *wrsm_regs)
{
	/*
	 * wci_qlim_3req_priority, wci_qlim_2req_priority,
	 * wci_qlim_sort_ciq, wci_qlim_sort_niq, and wci_qlim_sort_piq
	 * need to be updated if this cluster node is part of a
	 * wildcat SSM.
	 */
}


void
deactivate_link(safari_port_t portid, volatile uchar_t *wrsm_regs,
    linkid_t link_num)

{
	wci_sw_link_control_u link_control;
	uint64_t offset = link_num * STRIDE_WCI_SW_LINK_CONTROL;
	uint64_t cntlAddr = ADDR_WCI_SW_LINK_CONTROL + offset;

	DPRINTF(WP_DEBUG, (CE_CONT, "wci %d deactivate_link %d regs %p",
	    portid, link_num, (void *)wrsm_regs));

	link_control.val = read_reg(wrsm_regs, cntlAddr);

	link_control.bit.usr_data_1 = 0;
	link_control.bit.usr_data_2 = 0;
	link_control.bit.xmit_enable = 0;
	link_control.bit.laser_enable = 0;
	link_control.bit.link_state = LINK_STATE_OFF;

	write_reg(wrsm_regs, cntlAddr, link_control.val);
}

void
activate_link(wrsmplat_thread_data_t *data)
{
	wrsm_uplink_msg_t *msg = data->msg;
	volatile uchar_t *wrsm_regs = data->wrsm_regs;
	wci_sw_link_control_u link_control;
	wci_sw_link_status_u link_status;
	wci_sw_link_error_count_u error_count;
	wci_id_u wci_id_reg;
	uint64_t offset = msg->link_num * STRIDE_WCI_SW_LINK_CONTROL;
	uint64_t cntlAddr = ADDR_WCI_SW_LINK_CONTROL + offset;
	uint64_t statAddr = ADDR_WCI_SW_LINK_STATUS + offset;
	uint64_t eCntAddr = ADDR_WCI_SW_LINK_ERROR_COUNT + offset;
#ifdef DEBUG
	uint64_t pa, kpf;
#endif /* DEBUG */
	gnid_t remote_gnid;
	/* LINTED */
	fmnodeid_t remote_fmnodeid;
	linkid_t remote_link;
	safari_port_t remote_port;
	safari_port_t wci_id = msg->wci_port_id;
	linkid_t link_id = msg->link_num;

	link_control.val = read_reg(wrsm_regs, cntlAddr);

	link_control.bit.usr_data_1 = 0;
	link_control.bit.usr_data_2 = 0;
	link_control.bit.ustat_src = 0;
	link_control.bit.xmit_enable = 0;
	link_control.bit.paroli_tck_enable = 0;
	link_control.bit.near_end_shutdown_lock = 0;
	link_control.bit.laser_enable = 1;
	link_control.bit.error_inducement = 0;
	link_control.bit.auto_shut_en = 0;
	link_control.bit.rexmit_shutdown_en = 0;
	write_reg(wrsm_regs, cntlAddr, link_control.val);

	wci_id_reg.val = read_reg(wrsm_regs, ADDR_WCI_ID);
	if (wci_id_reg.val != WCI_ID_WCI2) {
	    write_reg(wrsm_regs, ADDR_WCI_SW_LINK_REXMIT + offset, 0);
	}

#ifdef DEBUG
	kpf = hat_getkpfnum((caddr_t)wrsm_regs);
	pa = (kpf << 13) | (((uint64_t)wrsm_regs) & 0x1fff);
	DPRINTF(WP_DEBUG, (CE_CONT, "trying to bring up wci %d link %d "
	    "regs=%p pa=%p", wci_id, link_id, (void *)wrsm_regs,
	    (void *)pa));
#endif

	while (try_activate(msg, wrsm_regs) != 0) {
		delay(drv_usectohz(250 * 1000));
		if (check_stop_thread(wci_id, link_id)) {
			kmem_free(msg, sizeof (wrsm_uplink_msg_t));
			exit_thread(wci_id, link_id);
		}
	}

	/*
	 * Set the link to in use mode and, turn on the transmitters,
	 * and clear out the error count.
	 */
	link_control.val = read_reg(wrsm_regs, cntlAddr);
	link_control.bit.xmit_enable = 1;
	write_reg(wrsm_regs, cntlAddr, link_control.val);
	error_count.val = 0;
	write_reg(wrsm_regs, eCntAddr, error_count.val);

	DPRINTF(WP_DEBUG, (CE_NOTE, "link %d wci %d is up",
	    msg->link_num, msg->wci_port_id));


	/*
	 * We can send 16 bits to the far side via usr_data_1, it is
	 * encoded as follows:
	 * 0-7:   gnid
	 * 8-12:  portid
	 * 13-15: linknum
	 */
	link_control.val = read_reg(wrsm_regs, cntlAddr);
	link_control.bit.usr_data_1 = (msg->config_data.gnid & 0xff) |
	    ((msg->wci_port_id & 0x1f) << 8) |
	    ((msg->link_num & 0x7) << 13);
	link_control.bit.usr_data_2 = 1;
	write_reg(wrsm_regs, cntlAddr, link_control.val);

	DPRINTF(WP_LINK, (CE_NOTE, "link %d sending discovery data %x",
	    msg->link_num, link_control.bit.usr_data_1));

	/*
	 * Spin waiting for the remote side to set the user_data_2
	 */
	link_status.val = read_reg(wrsm_regs, statAddr);
	while (link_status.bit.farend_ustat_2 < 1) {
		delay(drv_usectohz(100 * 1000));
		if (check_stop_thread(wci_id, link_id)) {
			kmem_free(msg, sizeof (wrsm_uplink_msg_t));
			exit_thread(wci_id, link_id);
		}

		link_status.val = read_reg(wrsm_regs, statAddr);
	}

	DPRINTF(WP_LINK, (CE_NOTE, "link %d got discovery data %x",
	    msg->link_num, link_status.bit.farend_ustat_1));

	remote_gnid = link_status.bit.farend_ustat_1 & 0xff;
	remote_port = (link_status.bit.farend_ustat_1 >> 8) & 0x1f;
	remote_link = (link_status.bit.farend_ustat_1 >> 13) & 0x7;

	DPRINTF(WP_DEBUG, (CE_NOTE, "wci %d remote side responds "
	    "gnid %d port %d link %d", msg->wci_port_id, remote_gnid,
	    remote_port, remote_link));

	/* XXX - just to be sure the link settles down */
	delay(drv_usectohz(200 * 1000));
	if (check_stop_thread(wci_id, link_id)) {
		kmem_free(msg, sizeof (wrsm_uplink_msg_t));
		exit_thread(wci_id, link_id);
	}

	link_control.val = read_reg(wrsm_regs, cntlAddr);
	link_control.bit.usr_data_2 = 2;
	link_control.bit.link_state = LINK_STATE_IN_USE;
	link_control.bit.auto_shut_en = 1;
	link_control.bit.rexmit_shutdown_en = 1;

	write_reg(wrsm_regs, cntlAddr, link_control.val);
	link_status.val = read_reg(wrsm_regs, statAddr);
	while (link_status.bit.farend_ustat_2 < 2) {
		delay(drv_usectohz(100 * 1000));
		if (check_stop_thread(wci_id, link_id)) {
			kmem_free(msg, sizeof (wrsm_uplink_msg_t));
			exit_thread(wci_id, link_id);
		}
		link_status.val = read_reg(wrsm_regs, statAddr);
	}

	/*
	 * Don't make the callback if the link is supposed to
	 * be in loopback mode.
	 */
	if (!data->loopback) {
		(*wrsmplat_state.cb_ops->link_up)(msg->wci_port_id,
		    (linkid_t)msg->link_num, msg->config_data.fmnodeid,
		    remote_gnid,
		    (linkid_t)remote_link, remote_port,
		    msg->config_data.partition_version,
		    msg->config_data.partition_id);
	} else {
		DPRINTF(WP_DEBUG, (CE_WARN, "Setting wci %d link %d IN_USE",
		    msg->wci_port_id, msg->link_num));
		link_control.val = read_reg(wrsm_regs, cntlAddr);
		link_control.bit.link_state = LINK_STATE_IN_USE;
		write_reg(wrsm_regs, cntlAddr, link_control.val);
	}

	kmem_free(msg, sizeof (wrsm_uplink_msg_t));
	kmem_free(data, sizeof (wrsmplat_thread_data_t));
	exit_thread(wci_id, link_id);
}


void
set_tnid(volatile uchar_t *regs, int link, int tnid)
{
	wci_fo_tnid_map_u tnid_map;

	mutex_enter(&wrsmplat_state.tnid_map_lock);
	tnid_map.val = read_reg(regs, ADDR_WCI_FO_TNID_MAP);
	switch (link) {
	case 0:
		tnid_map.bit.link0_tnid = tnid;
		break;
	case 1:
		tnid_map.bit.link1_tnid = tnid;
		break;
	case 2:
		tnid_map.bit.link2_tnid = tnid;
		break;
	default:
		DPRINTF(WP_DEBUG, (CE_WARN, "set_tnid: illegal link num: %d",
		    link));
		break;
	}
	write_reg(regs, ADDR_WCI_FO_TNID_MAP, tnid_map.val);
	/* read back to commit the write */
	tnid_map.val = read_reg(regs, ADDR_WCI_FO_TNID_MAP);
	mutex_exit(&wrsmplat_state.tnid_map_lock);
}

int
try_activate(wrsm_uplink_msg_t *msg, volatile uchar_t *wrsm_regs)
{
	wci_sw_link_control_u link_control;
	wci_sw_link_status_u link_status;
	wci_sw_link_error_count_u error_count;
	wci_sw_esr_u sw_esr, sw_esr_mask;
	uint64_t offset = msg->link_num * STRIDE_WCI_SW_LINK_CONTROL;
	uint64_t cntlAddr = ADDR_WCI_SW_LINK_CONTROL + offset;
	uint64_t statAddr = ADDR_WCI_SW_LINK_STATUS + offset;
	uint64_t eCntAddr = ADDR_WCI_SW_LINK_ERROR_COUNT + offset;
	int count, retry;
	int checks;
	int good_tnid;
	int tnid;
	safari_port_t wci_id = msg->wci_port_id;
	linkid_t link_id = msg->link_num;

	if (link_id >= WRSM_LINKS_PER_WCI) {
		DPRINTF(WP_DEBUG, (CE_NOTE, "try_activate: bad data link=%d "
		    "offset = %" PRIx64, link_id, offset));
		kmem_free(msg, sizeof (wrsm_uplink_msg_t));
		exit_thread(wci_id, link_id);
	}

	/* Clear error bits corresponding to this link */
	sw_esr_mask.val = 0;
	switch (msg->link_num) {
	case 0:
		sw_esr_mask.bit.acc_link_0_auto_shut = 1;
		sw_esr_mask.bit.acc_link_0_failover = 1;
		break;
	case 1:
		sw_esr_mask.bit.acc_link_1_auto_shut = 1;
		sw_esr_mask.bit.acc_link_1_failover = 1;
		break;
	case 2:
		sw_esr_mask.bit.acc_link_2_auto_shut = 1;
		sw_esr_mask.bit.acc_link_2_failover = 1;
		break;
	}
	sw_esr.val = read_reg(wrsm_regs, ADDR_WCI_SW_ESR);
	sw_esr.val &= sw_esr_mask.val;
	write_reg(wrsm_regs, ADDR_WCI_SW_ESR, sw_esr.val);

	/* Toggle the link state OFF/SEEK */
	link_control.val = read_reg(wrsm_regs, cntlAddr);
	link_control.bit.link_state = LINK_STATE_OFF;
	write_reg(wrsm_regs, cntlAddr, link_control.val);

	link_control.val = read_reg(wrsm_regs, cntlAddr);
	link_control.bit.link_state = LINK_STATE_SEEK;
	write_reg(wrsm_regs, cntlAddr, link_control.val);
	/* read it back to make sure the write happened */
	link_control.val = read_reg(wrsm_regs, cntlAddr);
	DPRINTF(WP_DEBUG, (CE_CONT, "try_activate: "
	    "Trying to bring up link %d wci %d",
	    msg->link_num, msg->wci_port_id));

	/*
	 * The max paroli reset setup time is 100ms.  Putting the link
	 * into STATE_SEEK brings the paroli out of reset, so we have
	 * to wait 100ms before we can expect anything useful from the
	 * status register
	 */
	delay(drv_usectohz(100 * 1000));

	/* Wait for optical_signal_detect to come on */
	link_status.val = read_reg(wrsm_regs, statAddr);
	count = 0;
	while (link_status.bit.optical_signal_detect == 0) {
		DPRINTF(WP_THREAD, (CE_CONT, "try_activate: no opt sig"));
		++count;
		if (count >= 200) {
			DPRINTF(WP_LINK, (CE_CONT, "Waiting for optical "
			    "signal detect link %d wci %d",
			    msg->link_num, msg->wci_port_id));
			return (-1);
		}
		/* sleep 250ms */
		delay(drv_usectohz(250 * 1000));
		if (check_stop_thread(wci_id, link_id)) {
			kmem_free(msg, sizeof (wrsm_uplink_msg_t));
			exit_thread(wci_id, link_id);
		}
		link_status.val = read_reg(wrsm_regs, statAddr);
	}
	DPRINTF(WP_LINK, (CE_CONT, "try_activate: opt sig detect: %u",
	    link_status.bit.optical_signal_detect));

	good_tnid = B_FALSE;

	for (tnid = 0; tnid < 16; ++tnid) {
		DPRINTF(WP_LINK, (CE_CONT, "try_activate: "
		    "Trying tnid %d", tnid));
		set_tnid(wrsm_regs, msg->link_num, tnid);
		/*
		 * There is some probability <1 that the end_status
		 * will be reported correctly in the link_status
		 * register.  Dave Saterfield guesses ~60% but it
		 * could be a lot worse if there is much clock skew
		 * between machines.  It should only give false
		 * negative, not false positive results.  So we try a
		 * few times and if it ever says "ready" then we declare
		 * success.
		 */
		for (retry = 0; retry < 5; ++retry) {
			link_status.val = read_reg(wrsm_regs, statAddr);
			if (link_status.bit.end_status ==
			    END_STATUS_NEAR_READY ||
			    link_status.bit.end_status ==
			    END_STATUS_ALL_READY) {
				good_tnid = B_TRUE;
				break;
			}
		}
		if (good_tnid) {
			DPRINTF(WP_THREAD, (CE_CONT, "Good tnid found!"));
			break;
		}
	}
#ifdef DEBUG
	if (tnid == 16) {
		DPRINTF(WP_LINK, (CE_CONT, "Didn't find good tnid status %u "
		    "link %d wci %d", link_status.bit.end_status,
		    msg->link_num, msg->wci_port_id));
	} else {
		DPRINTF(WP_LINK, (CE_CONT, "Set tnid to %d, status %u "
		    "link %d wci %d retries %d", tnid,
		    link_status.bit.end_status, msg->link_num,
		    msg->wci_port_id, retry));
	}
#endif /* DEBUG */

	/*
	 * If we didn't find the right tnid, the link must be in a bad
	 * state, so reset and try again.
	 */
	if (!good_tnid)
		return (-1);

	for (checks = 15; checks > 0; checks--) {
		link_status.val = read_reg(wrsm_regs, statAddr);
		if (link_status.bit.end_status == END_STATUS_ALL_READY) {
			error_count.val = 0;
			write_reg(wrsm_regs, eCntAddr, error_count.val);
#ifndef NEW_PAROLIS_ONLY
			delay(drv_usectohz(3000 * 1000));
#endif /* NEW_PAROLIS_ONLY */
			if (check_stop_thread(wci_id, link_id)) {
				kmem_free(msg, sizeof (wrsm_uplink_msg_t));
				exit_thread(wci_id, link_id);
			}

			for (checks = 5; checks > 0; checks--) {
				link_status.val = read_reg(wrsm_regs,
				    statAddr);
				if (link_status.bit.end_status !=
				    END_STATUS_ALL_READY)
					continue;

				error_count.val = read_reg(wrsm_regs,
				    eCntAddr);
				/*
				 * if it's still up and error count is
				 * not too high, declare success
				 */
				if (error_count.bit.error_count < 10) {
					return (0);
				} else {
					DPRINTF(WP_DEBUG, (CE_CONT,
					    "high error count %" PRIx64
					    " status = %" PRIx64,
					    error_count.val,
					    link_status.val));
					return (-1);
				}
			}
		}
		delay(drv_usectohz(100 * 1000));
		if (check_stop_thread(wci_id, link_id)) {
			kmem_free(msg, sizeof (wrsm_uplink_msg_t));
			exit_thread(wci_id, link_id);
		}
	}
	return (-1);
}

/*
 * Verifies valid stripegroup. For Serengeti, all wcis must be in same
 * SSM node.
 */
int
wrsmplat_stripegroup_verify(const wrsm_stripe_group_t *sg)
{
	int ssmnode;
	int i;

	if (sg->nwcis < 2) {
		return (0);
	}
	ssmnode = PORT2NODE(sg->wcis[0]);
	for (i = 0; i < sg->nwcis; i++) {
		if (ssmnode != PORT2NODE(sg->wcis[i])) {
			return (ENOTSUP);
		}
	}
	return (0);
}

/*
 * If in an SSM, configures SSM-mode WCIs to route appropriate ncslices
 * to the right local domain. For non-SSM Serengeti, no action is required.
 */
/* ARGSUSED */
void
wrsmplat_ncslice_setup(wrsm_ncowner_map_t owner[WRSM_MAX_NCSLICES])
{
	/* XXX - SSM case not yet implemented */
}

/*
 * Enter ncslice update critical section - no action for non-SSM Serengeti.
 */
void
wrsmplat_ncslice_enter(void)
{
}

/*
 * Exit ncslice update critical section - no action for non-SSM Serengeti.
 */
void
wrsmplat_ncslice_exit(void)
{
}

/* Does a cross-trap sync */
void
wrsmplat_xt_sync(int cpu_id)
{
	cpuset_t cpuset;
	CPUSET_ZERO(cpuset);
	CPUSET_ADD(cpuset, cpu_id);
	xt_sync(cpuset);
}

/* Support for suspend/resume */

void
wrsmplat_suspend(safari_port_t wci)
{
	if (!wrsm_use_sgsbbc) {
		int i;
		int link;
		for (i = 0; i < WRSM_MAX_WCIS; i++) {
			if (wrsmplat_state.wci[i].valid &&
			    wrsmplat_state.wci[i].wci_id == wci) {
				if (wrsmplat_state.wci[i].suspended) {
					return;
				}
				wrsmplat_state.wci[i].suspended = B_TRUE;
				break;
			}
		}
		for (link = 0; link < WRSM_MAX_LINKS_PER_WCI; link++) {
			stop_thread(wci, link);
		}
	}
}

void
wrsmplat_resume(safari_port_t wci)
{
	/*
	 * No need to restart link bring-up thread, since driver will
	 * eventually time-out and retry later.
	 */
	if (!wrsm_use_sgsbbc) {
		int i;
		for (i = 0; i < WRSM_MAX_WCIS; i++) {
			if (wrsmplat_state.wci[i].valid &&
			    wrsmplat_state.wci[i].wci_id == wci) {
				wrsmplat_state.wci[i].suspended = B_FALSE;
				break;
			}
		}
	}
}
