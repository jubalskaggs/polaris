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

#pragma ident	"@(#)iscsi_conn.c	1.1	06/06/20 SMI"

#include <signal.h>
#include <pthread.h>
#include <assert.h>
#include <sys/select.h>
#include <stdlib.h>
#include <poll.h>
#include <strings.h>
#include <sys/filio.h>
#include <errno.h>
#include <utility.h>
#include <unistd.h>
#include <sys/stropts.h>
#include <syslog.h>
#include <sys/iscsi_protocol.h>

#include "local_types.h"
#include "iscsi_conn.h"
#include "iscsi_sess.h"
#include "iscsi_login.h"
#include "iscsi_ffp.h"
#include "utility.h"
#include "target.h"
#include "port.h"
#include "t10.h"

/*
 * defined here so that pad_text is initialized to zero's. It's
 * never modified.
 */
static const char pad_text[ISCSI_PAD_WORD_LEN] = { 0 };

static void iscsi_conn_data_rqst(t10_cmd_t *cmd);
static void iscsi_conn_cmdcmplt(t10_cmd_t *cmd);
static void iscsi_conn_data_in(t10_cmd_t *);
static void iscsi_conn_pkt(iscsi_conn_t *c, iscsi_rsp_hdr_t *in);

static void send_datain_pdu(iscsi_conn_t *c, t10_cmd_t *cmd,
    uint8_t final_flag);
static void send_scsi_rsp(iscsi_conn_t *c, t10_cmd_t *cmd);
static void queue_noop_in(iscsi_conn_t *c);
static char *state_to_str(iscsi_state_t s);
static void send_async_logout(iscsi_conn_t *c);
static void send_async_scsi(iscsi_conn_t *c, int key, int asc, int ascq);

void *
conn_poller(void *v)
{
	iscsi_conn_t	*c = (iscsi_conn_t *)v;
	int		nbytes,
			pval;
	nfds_t		nfds = 1;
	struct pollfd	fds[1];
	iscsi_state_t	state;
	target_queue_t	*mgmtq = c->c_mgmtq;

	fds[0].fd	= c->c_fd;
	fds[0].events	= POLLIN;

	util_title(c->c_mgmtq, Q_CONN_LOGIN, c->c_num, "Start Poller");
	while ((pval = poll(fds, nfds, 30 * 1000)) != -1) {

		/*
		 * The true asynchronous events are when we're in S5_LOGGED_IN
		 * state. In the iscsi_full_feature() code the state is
		 * locked and checked before sending any messages along. The
		 * mutex is grabbed here only to prevent a collision between
		 * some thread setting the state and our reading of the value.
		 * There's no harm in us grabbing the state which might
		 * change right after we unlock the mutex.
		 */
		(void) pthread_mutex_lock(&c->c_state_mutex);
		state = c->c_state;
		(void) pthread_mutex_unlock(&c->c_state_mutex);

		switch (state) {
		case S1_FREE:
			/*
			 * If we moved to the free state. The session
			 * was sent a message to shutdown. Once it
			 * completes it will reply with a shutdown
			 * response which will close the main
			 * connection thread. So, this thread just
			 * returns a stop processing incoming packets.
			 */
			goto error;

		case S3_XPT_UP:
			if (ioctl(c->c_fd, FIONREAD, &nbytes) < 0) {
				queue_message_set(c->c_dataq, 0, msg_shutdown,
				    0);
				goto error;
			}

			/*
			 * To be fully compliant the code should use
			 * ioctl(fd, I_PEEK, (struct strpeek v)); and
			 * look to see if the header is indeed a login
			 * packet. If not, just close the connection.
			 */
			if (nbytes < sizeof (iscsi_login_hdr_t)) {
				queue_message_set(c->c_dataq, 0,
				    msg_shutdown, 0);
				goto error;
			} else {
				/*
				 * Change the state to S4_IN_LOGIN.
				 * Since we haven't touched the data
				 * waiting on the stream when the
				 * sema_post() occurs below the poller
				 * will find data again and send
				 * another packet ready message at
				 * which point we deal with the
				 * login.
				 */
				conn_state(c, T4);
			}
			break;

		case S4_IN_LOGIN:
			if (iscsi_handle_login_pkt(c) == False)
				goto error;
			break;

		case S7_LOGOUT_REQUESTED:
		case S5_LOGGED_IN:
			if (fds[0].revents & POLLIN) {

				if (iscsi_full_feature(c) == False)
					goto error;

			} else if (c->c_sess->s_type == SessionNormal) {
				queue_prt(c->c_mgmtq, Q_CONN_NONIO,
				    "CON%x  Send a NOP request", c->c_num);
				queue_noop_in(c);
			}
			break;

		case S6_IN_LOGOUT:
			goto error;

		case S8_CLEANUP_WAIT:
			queue_prt(c->c_mgmtq, Q_CONN_ERRS,
			    "Haven't handled state S8");
			queue_message_set(c->c_dataq, 0,
			    msg_shutdown_rsp, 0);
			goto error;
		}

	}

error:
	/*
	 * Only when we're logged in would we have an active session
	 * which needs to be shut down. In the case of S4_IN_LOGIN we could
	 * transition to either S1_FREE in which case a shutdown message
	 * was sent to the session which in turn will reply with a shutdown
	 * response causing the conn_process to exit.
	 */
	if (c->c_state == S5_LOGGED_IN)
		conn_state(c, T8);

	/*
	 * If a msg_conn_lost was already sent it's invalid to reference
	 * the management queue from the connection structure at this point.
	 */
	util_title(mgmtq, Q_CONN_LOGIN, c->c_num, "End Poller");

	if (pval == -1)
		queue_message_set(c->c_dataq, 0, msg_conn_lost, 0);
	return (NULL);
}

/*
 * conn_process -- thread which runs a connection
 */
void *
conn_process(void *v)
{
	iscsi_conn_t	*c		= (iscsi_conn_t *)v;
	iscsi_cmd_t	*cmd;
	Boolean_t	process		= True,
			is_last		= False;
	msg_t		*m;
	void		*thr_status;
	int		i;
	mgmt_request_t	*mgmt;
	char		debug[80];
	time_t		tval = time((time_t *)0);

	c->c_dataq	= queue_alloc();
	c->c_maxcmdsn	= CMD_MAXOUTSTANDING;
	if (sema_init(&c->c_datain, 0, USYNC_THREAD, NULL) != 0) {
		port_conn_remove(c);
		free(c);
		return (NULL);
	}

	util_title(c->c_mgmtq, Q_CONN_LOGIN, c->c_num, "Start Receiver");
	util_title(c->c_mgmtq, Q_CONN_LOGIN, c->c_num,
	    ctime_r(&tval, debug, sizeof (debug)));

	(void) pthread_create(&c->c_thr_id_poller, NULL,
	    conn_poller, (void *)c);
	c->c_thr_id_process = pthread_self();

	assert(c->c_state == S1_FREE);
	conn_state(c, T3);

	do {
		m = queue_message_get(c->c_dataq);
		switch (m->msg_type) {
		case msg_mgmt_rqst:
			mgmt = (mgmt_request_t *)m->msg_data;
			if (c->c_state == S5_LOGGED_IN) {
				if (mgmt->m_request == mgmt_logout) {
					conn_state(c, T11);
					queue_message_set(mgmt->m_q, 0,
					    msg_mgmt_rply, 0);
				} else {
					queue_message_set(c->c_sessq, 0,
					    msg_mgmt_rqst, m->msg_data);
				}
			} else {
				/*
				 * Corner case which can occur when the
				 * connection has just started and is in the
				 * process of logging in and we get a
				 * mangement request. There's no session
				 * information or even a queue setup. Just
				 * show an empty connection.
				 *
				 * For the mgmt_logout, it's possible that
				 * we sent the T11 state change causing the
				 * connection to enter the S7 state. If we
				 * get a logout request again the specification
				 * says to just drop the connection.
				 */
				if (mgmt->m_request == mgmt_logout)
					conn_state(c, T18);
				else {
					(void) pthread_mutex_lock(
						&mgmt->m_resp_mutex);
					xml_add_tag(mgmt->m_u.m_resp,
					    "connection", NULL);
					(void) pthread_mutex_unlock(
						&mgmt->m_resp_mutex);
				}
				queue_message_set(mgmt->m_q, 0,
				    msg_mgmt_rply, 0);
			}
			m->msg_data = NULL;
			break;

		case msg_conn_lost:
			queue_prt(c->c_mgmtq, Q_CONN_LOGIN,
			    "CON%x  Shutdown: connection", c->c_num);

			if (c->c_state == S5_LOGGED_IN)
				conn_state(c, T8);
			break;

		case msg_shutdown_rsp:
			if (c->c_state == S6_IN_LOGOUT)
				conn_state(c, T13);
			(void) pthread_join(c->c_thr_id_poller, &thr_status);
			is_last		= (Boolean_t)m->msg_data;
			m->msg_data	= NULL;
			process		= False;
			break;

		case msg_shutdown:
			if (c->c_state == S5_LOGGED_IN) {
				conn_state(c, T8);
			} else if (c->c_state == S4_IN_LOGIN)
				conn_state(c, T7);
			else {
				process = False;
			}
			break;

		case msg_targ_inventory_change:
			if (c->c_state == S5_LOGGED_IN) {
				send_async_scsi(c, KEY_UNIT_ATTENTION, 0x3f,
				    0x0e);
			}
			break;

		case msg_send_pkt:
			iscsi_conn_pkt(c, (iscsi_rsp_hdr_t *)m->msg_data);
			break;

		case msg_cmd_data_rqst:
			/*
			 * The STE needs more data to complete
			 * the write command.
			 */
			iscsi_conn_data_rqst((t10_cmd_t *)m->msg_data);
			break;

		case msg_cmd_data_in:
			/*
			 * Data is available to satisfy the READ command
			 */
			iscsi_conn_data_in((t10_cmd_t *)m->msg_data);
			break;

		case msg_cmd_cmplt:
			/*
			 * Status is available for a previous STEOut.
			 * The status may be good and the previous STEOut data
			 * wasn't sent so we phase collapse.
			 */
			iscsi_conn_cmdcmplt((t10_cmd_t *)m->msg_data);
			break;

		default:
			queue_prt(c->c_mgmtq, Q_CONN_ERRS,
			    "CON%x  Didn't handle msg_type %d", c->c_num,
			    m->msg_type);
			break;
		}

		queue_message_free(m);
	} while (process == True);

	/*
	 * Free any resources used.
	 */
	if (c->c_text_area)
		free(c->c_text_area);
	if (c->c_fd != -1)
		(void) close(c->c_fd);

	/*
	 * It's possible, but very unlikely that c_sessq is NULL at this
	 * point. I saw one case where the system had problems causing the
	 * poller routine to exit real early so that the session was never
	 * setup causing the daemon to get a SEGV in queue_free when a NULL
	 * was passed in.
	 */
	if ((is_last == True) && (c->c_sessq != NULL))
		queue_free(c->c_sessq, sess_queue_data_remove);

	/*
	 * See if there are any commands outstanding and free them.
	 * NOTE: Should walk through the data_ptr list and find data structure
	 * who have alligence to this connection and free them as well.
	 */
	(void) pthread_mutex_lock(&c->c_mutex);

	for (i = 0, cmd = c->c_cmd_head; cmd; i++)
		cmd = cmd->c_next; /* debug count of lost ttt's */

	(void) snprintf(debug, sizeof (debug), "CON%x  %d Lost TTTs: ",
	    c->c_num, i);

	for (i = 0, cmd = c->c_cmd_head; cmd; i++) {
		iscsi_cmd_t *n = cmd->c_next;
		if (cmd->c_state != CmdCanceled) {
			(void) snprintf(debug + strlen(debug),
			    sizeof (debug) - strlen(debug),
			    "0x%x ", cmd->c_ttt);
		}
		free(cmd);
		cmd = n;
	}
	(void) pthread_mutex_unlock(&c->c_mutex);

	if (i) {
		/*
		 * If there where any found send a message indicating
		 * which ones. This message is purely for information
		 * and is not indicative of an error.
		 */
		queue_prt(c->c_mgmtq, Q_CONN_LOGIN, debug);
	}

	if (c->c_cmds_avg_cnt != 0)
		queue_prt(c->c_mgmtq, Q_CONN_LOGIN,
		    "CON%x  Average completion %lldms",  c->c_num,
		    (c->c_cmds_avg_sum / c->c_cmds_avg_cnt) / (1000 * 1000));

	(void) sema_destroy(&c->c_datain);
	if (c->c_targ_alias)
		free(c->c_targ_alias);

	util_title(c->c_mgmtq, Q_CONN_LOGIN, c->c_num, "End Receiver");

	/*
	 * Remove this connection from linked list of current connections.
	 * This will also free the connection queue. Must not hold the
	 * q here because port_conn_remove-->queue_free->conn_queue_data
	 * will possible grab the mutex.
	 */
	port_conn_remove(c);
	free(c);
	return (NULL);
}

/*
 * []----
 * | iscsi_conn_pkt -- send out PDU from receive thread
 * |
 * | (1) This PDU could be either:
 * |     (a) A NOP request was sent from the initiator which the recieve
 * |         side processed and is requesting to be sent back.
 * |     (b) Nothing has been received in N seconds and we're looking
 * |         to see if the connection is still alive.
 * |     (c) A task management request was processed by the receive side
 * |         and the response must be sent.
 * | (2) Fields to be filled in
 * |     Need to delay filling in several of the fields until
 * |     now to avoid using sequence number which would be out of
 * |     order.
 * []----
 */
static void
iscsi_conn_pkt(iscsi_conn_t *c, iscsi_rsp_hdr_t *in)
{
	if (c->c_state != S5_LOGGED_IN) {
		free(in);
		return;
	}
	(void) pthread_mutex_lock(&c->c_mutex);
	in->statsn	= htonl(c->c_statsn++);
	(void) pthread_mutex_lock(&c->c_sess->s_mutex);
	in->expcmdsn	= htonl(c->c_sess->s_seencmdsn + 1);
	in->maxcmdsn	= htonl(iscsi_cmd_window(c) + c->c_sess->s_seencmdsn);
	(void) pthread_mutex_unlock(&c->c_sess->s_mutex);
	(void) pthread_mutex_unlock(&c->c_mutex);

	send_iscsi_pkt(c, (iscsi_hdr_t *)in, 0);
	free(in);
}

/*
 * []----
 * | iscsi_conn_data_rqst -- request that data be sent from the initiator
 * []----
 */
static void
iscsi_conn_data_rqst(t10_cmd_t *t)
{
	iscsi_cmd_t	*cmd	= T10_TRANS_ID(t);
	iscsi_conn_t	*c;
	iscsi_rtt_hdr_t	rtt;

	bzero(&rtt, sizeof (rtt));

	c = cmd->c_allegiance;
	(void) pthread_mutex_lock(&c->c_mutex);
	(void) pthread_mutex_lock(&c->c_state_mutex);
	if ((c->c_state != S5_LOGGED_IN) ||
	    (cmd->c_state == CmdCanceled)) {
		t10_cmd_state(t, T10_Cmd_Event_Release);
		(void) pthread_mutex_unlock(&c->c_state_mutex);
		(void) pthread_mutex_unlock(&c->c_mutex);
		return;
	}
	(void) pthread_mutex_unlock(&c->c_state_mutex);

	/*
	 * Save the data pointer from the emulation code. It's their
	 * responsibility to allocate space for the data which the
	 * initiator will return. When we receive a DATAOUT packet
	 * we'll copy data from the socket directly to this buffer.
	 */
	cmd->c_data	= T10_DATA(t);

	/*
	 * RFC3270.10.8.3
	 * The statsn field will contain the next statsn. The statsn for this
	 * connection is not advanced after this PDU is sent.
	 */
	rtt.statsn = htonl(c->c_statsn);

	rtt.opcode	= ISCSI_OP_RTT_RSP;
	rtt.flags	= ISCSI_FLAG_FINAL;
	rtt.itt		= cmd->c_itt;
	rtt.ttt		= cmd->c_ttt;
	rtt.data_offset = htonl(T10_DATA_OFFSET(t));
	rtt.data_length = htonl(T10_DATA_LEN(t));

	(void) pthread_mutex_lock(&c->c_sess->s_mutex);
	rtt.maxcmdsn = htonl(iscsi_cmd_window(c) + c->c_sess->s_seencmdsn);
	rtt.expcmdsn = htonl(c->c_sess->s_seencmdsn + 1);
	(void) pthread_mutex_unlock(&c->c_sess->s_mutex);

#ifdef FULL_DEBUG
	queue_prt(c->c_mgmtq, Q_CONN_IO,
	    "CON%x  TTT 0x%x R2T offset 0x%x, len 0x%x",
	    c->c_num, cmd->c_ttt, T10_DATA_OFFSET(t), T10_DATA_LEN(t));
#endif

	t10_cmd_state(t, T10_Cmd_Event_DataOut_Sent);
	(void) pthread_mutex_unlock(&c->c_mutex);
	send_iscsi_pkt(c, (iscsi_hdr_t *)&rtt, 0);
}

/*
 * []----
 * | iscsi_conn_data_in -- Send data to initiator
 * []----
 */
void
iscsi_conn_data_in(t10_cmd_t *t)
{
	iscsi_cmd_t	*cmd		= (iscsi_cmd_t *)T10_TRANS_ID(t);
	iscsi_conn_t	*c;

	c = cmd->c_allegiance;
	(void) pthread_mutex_lock(&c->c_mutex);
	(void) pthread_mutex_lock(&c->c_state_mutex);
	if ((c->c_state != S5_LOGGED_IN) ||
	    (cmd->c_state == CmdCanceled)) {

		t10_cmd_state(t, T10_Cmd_Event_Release);
		while (cmd->c_t10_delayed) {
			t10_cmd_state(cmd->c_t10_delayed->id_t10_cmd,
			    T10_Cmd_Event_Release);
			iscsi_cmd_delayed_remove(cmd, cmd->c_t10_delayed);
		}
		(void) pthread_mutex_unlock(&c->c_state_mutex);
		(void) pthread_mutex_unlock(&c->c_mutex);
		return;
	}
	(void) pthread_mutex_unlock(&c->c_state_mutex);

	/*
	 * Need to deal with out of order data PDUs. RFC3720 allows
	 * the initiator to indicate if it can handle out-of-order
	 * PDUs.
	 */
	if (cmd->c_offset_in != T10_DATA_OFFSET(t)) {
		iscsi_cmd_delayed_store(cmd, t);
		(void) pthread_mutex_unlock(&c->c_mutex);
		return;
	}

	while (t != NULL) {
		cmd->c_offset_in += T10_DATA_LEN(t);
		if (T10_CMD_LAST(t) == True) {
			send_datain_pdu(c, t,
			    ISCSI_FLAG_FINAL | ISCSI_FLAG_DATA_STATUS);
			iscsi_cmd_free(c, cmd);
		} else {
			send_datain_pdu(c, t, 0);
		}
		t10_cmd_state(t, T10_Cmd_Event_Release);
		cmd->c_t10_cmd = NULL;

		if (cmd->c_t10_delayed &&
		    (cmd->c_t10_delayed->id_offset == cmd->c_offset_in)) {
			t = cmd->c_t10_delayed->id_t10_cmd;
			iscsi_cmd_delayed_remove(cmd, cmd->c_t10_delayed);
		} else {
			t = NULL;
		}
	}
	(void) pthread_mutex_unlock(&c->c_mutex);
}

/*
 * []----
 * | iscsi_conn_cmdcmplt -- Send out appropriate completion PDU
 * []----
 */
void
iscsi_conn_cmdcmplt(t10_cmd_t *t)
{
	iscsi_cmd_t	*cmd		= (iscsi_cmd_t *)T10_TRANS_ID(t);
	iscsi_conn_t	*c;

	c = cmd->c_allegiance;
	(void) pthread_mutex_lock(&c->c_mutex);
	(void) pthread_mutex_lock(&c->c_state_mutex);
	if ((c->c_state != S5_LOGGED_IN) ||
	    (cmd->c_state == CmdCanceled)) {

		t10_cmd_state(t, T10_Cmd_Event_Release);
		(void) pthread_mutex_unlock(&c->c_state_mutex);
		(void) pthread_mutex_unlock(&c->c_mutex);
		return;
	}
	(void) pthread_mutex_unlock(&c->c_state_mutex);

	if (T10_SENSE_LEN(t) || (T10_DATA(t) == 0)) {

		/*
		 * If d_sense_len is set there's a problem and we need to send
		 * a SCSI response packet. Or if there's no data buffer then
		 * this is an acknowledgement that a SCSI Write completed
		 * successfully.
		 */
		send_scsi_rsp(c, t);

	} else {

		/*
		 * send data out with final bit. Last packet of a SCSI
		 * READ Op and we'll send it out with the final/status
		 * bits set.
		 */
		send_datain_pdu(c, t,
		    ISCSI_FLAG_FINAL | ISCSI_FLAG_DATA_STATUS);

	}

	t10_cmd_state(t, T10_Cmd_Event_Release);
	cmd->c_t10_cmd = NULL;

	if (cmd->c_scb_extended != NULL)
		free(cmd->c_scb_extended);
	iscsi_cmd_free(c, cmd);
	(void) pthread_mutex_unlock(&c->c_mutex);
}

/*
 * []----
 * | send_datain_pdu -- Send DataIn PDU with READ data
 * |
 * | If this is the last read operation and it completed successfully
 * | the final flag will be set along with the status bit which indicates
 * | successful completion. This is known as a phase collapse for iSCSI.
 * |
 * | NOTE: connection mutex must be held.
 * []----
 */
static void
send_datain_pdu(iscsi_conn_t *c, t10_cmd_t *t, uint8_t final_flag)
{
	iscsi_cmd_t		*cmd = (iscsi_cmd_t *)T10_TRANS_ID(t);
	iscsi_data_rsp_hdr_t	rsp;

	assert(pthread_mutex_trylock(&c->c_mutex) != 0);
	bzero(&rsp, sizeof (rsp));

	rsp.opcode	= ISCSI_OP_SCSI_DATA_RSP;
	rsp.flags	= final_flag;
	rsp.cmd_status	= cmd->c_status;
	rsp.itt		= cmd->c_itt;
	rsp.ttt		= ISCSI_RSVD_TASK_TAG;
	rsp.datasn	= htonl(cmd->c_datasn++);
	rsp.offset	= htonl(T10_DATA_OFFSET(t));
	rsp.lun[1]	= (uint8_t)cmd->c_lun;

	hton24(rsp.dlength, T10_DATA_LEN(t));

	rsp.statsn	= htonl(c->c_statsn);
	/*
	 * The statsn is only incremented when the Status bit is set
	 * for a DataIn PDU. This must be done *after* the value
	 * was stored in the PDU.
	 */
	if (final_flag & ISCSI_FLAG_DATA_STATUS)
		c->c_statsn++;

	(void) pthread_mutex_lock(&c->c_sess->s_mutex);
	rsp.maxcmdsn = htonl(iscsi_cmd_window(c) + c->c_sess->s_seencmdsn);
	rsp.expcmdsn = htonl(c->c_sess->s_seencmdsn);
	(void) pthread_mutex_unlock(&c->c_sess->s_mutex);

	send_iscsi_pkt(c, (iscsi_hdr_t *)&rsp, T10_DATA(t));
}

/*
 * []----
 * | send_scsi_rsp -- Send SCSI reponse PDU
 * |
 * | NOTE: connection mutex must be held.
 * []----
 */
static void
send_scsi_rsp(iscsi_conn_t *c, t10_cmd_t *t)
{
	iscsi_cmd_t		*cmd = (iscsi_cmd_t *)T10_TRANS_ID(t);
	iscsi_scsi_rsp_hdr_t	rsp;
	void			*auto_sense = NULL;

	assert(pthread_mutex_trylock(&c->c_mutex) != 0);
	bzero(&rsp, sizeof (rsp));

	rsp.opcode	= ISCSI_OP_SCSI_RSP;
	rsp.flags	= ISCSI_FLAG_FINAL;
	rsp.itt		= cmd->c_itt;
	rsp.statsn	= htonl(c->c_statsn++);
	(void) pthread_mutex_lock(&c->c_sess->s_mutex);
	rsp.expcmdsn	= htonl(c->c_sess->s_seencmdsn + 1);
	rsp.maxcmdsn	= htonl(iscsi_cmd_window(c) + c->c_sess->s_seencmdsn);
	(void) pthread_mutex_unlock(&c->c_sess->s_mutex);
	rsp.cmd_status	= T10_CMD_STATUS(t);

	if (rsp.cmd_status) {
		rsp.response		= ISCSI_STATUS_CMD_COMPLETED;
		rsp.residual_count	= htonl(T10_CMD_RESID(t));
		if (cmd->c_writeop == True)
			rsp.flags |= ISCSI_FLAG_CMD_OVERFLOW;
		else
			rsp.flags |= ISCSI_FLAG_CMD_UNDERFLOW;

		if (T10_SENSE_LEN(t) != 0) {
			/*
			 * Need to handle autosense stuff. The data should
			 * be store in the d_sense area
			 */
			auto_sense = (void *)T10_SENSE_DATA(t);
			hton24(rsp.dlength, T10_SENSE_LEN(t));
		}
		queue_prt(c->c_mgmtq, Q_CONN_ERRS,
		    "CON%x  LUN%d SCSI Error Status: %d",
		    c->c_num, t->c_lu->l_common->l_num, rsp.cmd_status);
	} else {
		rsp.response	= ISCSI_STATUS_CMD_COMPLETED;
		rsp.expdatasn	= htonl(cmd->c_datasn);
	}

	send_iscsi_pkt(c, (iscsi_hdr_t *)&rsp, auto_sense);
}

static void
send_async_scsi(iscsi_conn_t *c, int key, int asc, int ascq)
{
	iscsi_async_evt_hdr_t		a;
	struct scsi_extended_sense	s;
	char				*buf;

	bzero(&a, sizeof (a));
	bzero(&s, sizeof (s));

	s.es_class	= CLASS_EXTENDED_SENSE;
	s.es_code	= CODE_FMT_FIXED_CURRENT;
	s.es_key	= key;
	s.es_valid	= 1;
	s.es_add_code	= asc;
	s.es_qual_code	= ascq;

	if ((buf = malloc(sizeof (s) + 2)) == NULL)
		return;

	buf[0] = (sizeof (s) >> 8) & 0xff;
	buf[1] = sizeof (s) & 0xff;
	bcopy(&s, &buf[2], sizeof (s));

	hton24(a.dlength, sizeof (s) + 2);
	a.opcode	= ISCSI_OP_ASYNC_EVENT;
	a.flags		= ISCSI_FLAG_FINAL;
	a.async_event	= ISCSI_ASYNC_EVENT_SCSI_EVENT;
	(void) pthread_mutex_lock(&c->c_mutex);
	a.statsn	= htonl(c->c_statsn++);
	(void) pthread_mutex_lock(&c->c_sess->s_mutex);
	a.expcmdsn	= htonl(c->c_sess->s_seencmdsn + 1);
	a.maxcmdsn	= htonl(iscsi_cmd_window(c) + c->c_sess->s_seencmdsn);
	(void) pthread_mutex_unlock(&c->c_sess->s_mutex);
	(void) pthread_mutex_unlock(&c->c_mutex);

	queue_prt(c->c_mgmtq, Q_CONN_NONIO,
	    "CON%x  Sending async scsi sense", c->c_num);

	send_iscsi_pkt(c, (iscsi_hdr_t *)&a, buf);
}

/*
 * []----
 * | send_async_logout -- request logout from initiator
 * []----
 */
static void
send_async_logout(iscsi_conn_t *c)
{
	iscsi_async_evt_hdr_t	a;

	bzero(&a, sizeof (a));

	a.opcode	= ISCSI_OP_ASYNC_EVENT;
	a.flags		= ISCSI_FLAG_FINAL;
	a.async_event	= ISCSI_ASYNC_EVENT_REQUEST_LOGOUT;
	(void) pthread_mutex_lock(&c->c_mutex);
	a.statsn	= htonl(c->c_statsn++);
	(void) pthread_mutex_lock(&c->c_sess->s_mutex);
	a.expcmdsn	= htonl(c->c_sess->s_seencmdsn + 1);
	a.maxcmdsn	= htonl(iscsi_cmd_window(c) + c->c_sess->s_seencmdsn);
	a.param3	= htons(ASYNC_LOGOUT_TIMEOUT);
	a.rsvd4[0]	= 0xff;
	a.rsvd4[1]	= 0xff;	/* According to the spec these four */
	a.rsvd4[2]	= 0xff;	/* values must be 0xff */
	a.rsvd4[3]	= 0xff;
	(void) pthread_mutex_unlock(&c->c_sess->s_mutex);
	(void) pthread_mutex_unlock(&c->c_mutex);

	queue_prt(c->c_mgmtq, Q_CONN_NONIO,
	    "CON%x  Sending async logout request", c->c_num);

	send_iscsi_pkt(c, (iscsi_hdr_t *)&a, 0);
}

/*
 * []----
 * | queue_noop_in -- generate a NOP request and queue it to be sent.
 * []----
 */
static void
queue_noop_in(iscsi_conn_t *c)
{
	iscsi_nop_in_hdr_t	*in;
	iscsi_cmd_t		*cmd = iscsi_cmd_alloc(c, ISCSI_OP_NOOP_IN);

	if (cmd == NULL)
		return;

	in = (iscsi_nop_in_hdr_t *)calloc(sizeof (*in), 1);
	if (in == NULL)
		return;

	in->opcode = ISCSI_OP_NOOP_IN | ISCSI_OP_IMMEDIATE;
	in->flags = ISCSI_FLAG_FINAL;
	in->ttt = cmd->c_ttt;
	in->itt = ISCSI_RSVD_TASK_TAG;

	iscsi_cmd_free(c, cmd);
	queue_message_set(c->c_dataq, 0, msg_send_pkt, (void *)in);
}

void
iscsi_capacity_change(char *targ_name, int lun)
{
	iscsi_conn_t		*conn;
	extern pthread_mutex_t	port_mutex;

	/*
	 * SBC-2 revision 16, section 4.6 -- Initialization
	 * Any time the parameter data returned by the READ CAPACITY(10)
	 * (see 5.10) or the READ CAPACITY(16) command (see 5.11) changes,
	 * the device server should establish a unit attention condition for
	 * the initiator port associated with each I_T nexus.
	 * Since the transport knows which initiators are currently accessing
	 * the target the message will be sent from here.
	 */
	(void) pthread_mutex_lock(&port_mutex);
	for (conn = conn_head; conn; conn = conn->c_next) {
		(void) pthread_mutex_lock(&conn->c_state_mutex);
		if ((conn->c_state == S5_LOGGED_IN) &&
		    (conn->c_sess->s_type == SessionNormal) &&
		    (strcmp(conn->c_sess->s_t_name, targ_name) == 0)) {

			queue_message_set(conn->c_sessq, 0,
			    msg_lu_capacity_change, (void *)(uintptr_t)lun);
		}
		(void) pthread_mutex_unlock(&conn->c_state_mutex);
	}
	(void) pthread_mutex_unlock(&port_mutex);
}

/*
 * []----
 * | iscsi_inventory_change -- Send notice to initiator that something changed.
 * []----
 */
void
iscsi_inventory_change(char *targ_name)
{
	iscsi_conn_t		*c;
	extern pthread_mutex_t	port_mutex;

	/*
	 * SPC-3 revision 21c, Section 6.21 REPORT_LUNS
	 * If the logical unit inventory changes for any reason
	 * (e.g. completion of initialization, removal of a logical unit,
	 * or create of a logical unit), then the device server shall generate
	 * a unit attention condition for all I_T nexuses, with the additional
	 * sense code set to REPORTED LUNS DATA HAS CHANGED.
	 */
	(void) pthread_mutex_lock(&port_mutex);
	for (c = conn_head; c; c = c->c_next) {
		(void) pthread_mutex_lock(&c->c_state_mutex);
		if ((c->c_state == S5_LOGGED_IN) &&
		    (c->c_sess->s_type == SessionNormal) &&
		    (strcmp(c->c_sess->s_t_name, targ_name) == 0)) {

			queue_prt(c->c_mgmtq, Q_CONN_NONIO,
			    "CON%x  Sending Inventory change out", c->c_num);
			/*
			 * Send a message indicating that the logical unit
			 * inventory has changed. 1) This message is sent
			 * to the session level which will pass it onto
			 * the SAM layer causing a UNIT_ATTENTION during
			 * the next command. 2) This message is also sent
			 * directly to the outgoing side of the connection
			 * which will send an asynchronous event message
			 * to the initiator.
			 */
			queue_message_set(c->c_sessq, 0,
			    msg_targ_inventory_change, 0);
			queue_message_set(c->c_dataq, 0,
			    msg_targ_inventory_change, 0);
		}
		(void) pthread_mutex_unlock(&c->c_state_mutex);
	}
	(void) pthread_mutex_unlock(&port_mutex);
}

/*
 * []----
 * | state_to_str -- return string for given state, used for debug
 * []----
 */
static char *
state_to_str(iscsi_state_t s)
{
	switch (s) {
	case S1_FREE: return ("FREE");
	case S3_XPT_UP: return ("XPT_UP");
	case S4_IN_LOGIN: return ("IN_LOGIN");
	case S5_LOGGED_IN: return ("LOGGED_IN");
	case S6_IN_LOGOUT: return ("IN_LOGOUT");
	case S7_LOGOUT_REQUESTED: return ("LOGOUT_REQUEST");
	case S8_CLEANUP_WAIT: return ("CLEANUP_WAIT");
	}
	return ("Unknown");
}

/*
 * []----
 * | event_to_str -- return string for given event, used for debug
 * []----
 */
static char *
event_to_str(iscsi_transition_t t)
{
	switch (t) {
	case T3: return ("T3");
	case T4: return ("T4");
	case T5: return ("T5");
	case T6: return ("T6");
	case T7: return ("T7");
	case T8: return ("T8");
	case T9: return ("T9");
	case T10: return ("T10");
	case T11: return ("T11");
	case T12: return ("T12");
	case T13: return ("T13");
	case T15: return ("T15");
	case T16: return ("T16");
	case T17: return ("T17");
	case T18: return ("T18");
	}
	return ("Unknown");
}

/*
 * []----
 * | conn_state -- Attempt to change from one state to the next
 * []----
 */
void
conn_state(iscsi_conn_t *c, iscsi_transition_t t)
{
	iscsi_state_t	old_state = c->c_state;

	(void) pthread_mutex_lock(&c->c_state_mutex);
	switch (c->c_state) {
	case S1_FREE:
		switch (t) {
		case T3:
			c->c_state = S3_XPT_UP;
			break;

		}
		break;

	case S3_XPT_UP:
		switch (t) {
		case T6:
			c->c_state = S1_FREE;
			break;

		case T4:
			c->c_statsn = 1;
			c->c_state = S4_IN_LOGIN;
			break;

		}
		break;

	case S4_IN_LOGIN:
		switch (t) {
		case T7:
			/*
			 * When there's a session a shutdown messages is
			 * sent giving the opportunity to free resources
			 * used by the session code and STE. Very early
			 * on a session might not exist when a failure
			 * occurs like getting a bad opcode. The connection
			 * process routine is going to sit around waiting
			 * for a message which will never come so fake
			 * a completion message here if there's no session.
			 */
			if (c->c_sessq == NULL) {
				queue_message_set(c->c_dataq, 0,
				    msg_shutdown_rsp, (void *)True);
			} else {
				queue_message_set(c->c_sessq, 0, msg_shutdown,
				    (void *)c);
			}
			c->c_state = S1_FREE;
			break;
		case T5:
			c->c_state = S5_LOGGED_IN;
			if (strncmp(c->c_sess->s_i_name,
			    "iqn.1991-05.com.microsoft",
			    strlen("iqn.1991-05.com.microsoft")) == 0)
				c->c_sess->s_cmdsn++;
			break;
		}
		break;

	case S5_LOGGED_IN:
		switch (t) {
		case T8:
			queue_message_set(c->c_sessq, 0, msg_shutdown,
			    (void *)c);
			c->c_state = S1_FREE;

			break;
		case T9:
			queue_message_set(c->c_sessq, 0, msg_shutdown,
			    (void *)c);
			c->c_state = S6_IN_LOGOUT;
			break;
		case T11:
			c->c_state = S7_LOGOUT_REQUESTED;
			send_async_logout(c);
			break;
		case T15:
			c->c_state = S8_CLEANUP_WAIT;
			break;
		}
		break;

	case S6_IN_LOGOUT:
		switch (t) {
		case T13:
			if (c->c_last_pkg) {
				send_iscsi_pkt(c, c->c_last_pkg, NULL);
				free(c->c_last_pkg);
			}
			c->c_state = S1_FREE;
			break;
		case T17:
			c->c_state = S8_CLEANUP_WAIT;
			break;
		}
		break;

	case S7_LOGOUT_REQUESTED:
		switch (t) {
		case T18:
			queue_message_set(c->c_sessq, 0, msg_shutdown,
			    (void *)c);
			c->c_state = S1_FREE;
			break;
		case T10:
			queue_message_set(c->c_sessq, 0, msg_shutdown,
			    (void *)c);
			c->c_state = S6_IN_LOGOUT;
			break;
		case T12:
			c->c_state = S7_LOGOUT_REQUESTED;
			break;
		case T16:
			c->c_state = S8_CLEANUP_WAIT;
			break;
		}
		break;

	case S8_CLEANUP_WAIT:
	default:
		break;
	}
	queue_prt(c->c_mgmtq, Q_CONN_NONIO, "CON%x  ---- %s(%s) -> %s",
	    c->c_num, state_to_str(old_state), event_to_str(t),
	    state_to_str(c->c_state));
	(void) pthread_mutex_unlock(&c->c_state_mutex);
}

/*
 * []----
 * | send_iscsi_pkt -- output PDU header, data, and alignment bytes if needed
 * |
 * | NOTE: This routine may be called with the connection mutex held. This
 * | is done to prevent a state change being made to a command pointer. This
 * | routine is currently written so that it doesn't need to have this mutex
 * | held or calls a routine which needs it to be held.
 * []----
 */
void
send_iscsi_pkt(iscsi_conn_t *c, iscsi_hdr_t *h, char *opt_text)
{
	int		dlen	= ntoh24(h->dlength),
			pad_len;
	uint32_t	crc;

	/*
	 * Sanity check. If there's a length in the header we must
	 * have text to send or if the length is zero there better not
	 * be any text.
	 */
	if (((dlen == 0) && (opt_text != NULL)) ||
	    ((dlen != 0) && (opt_text == NULL)))
		return;

	if (write(c->c_fd, h, sizeof (*h)) < 0) {
		if (errno == EPIPE) {

			/*
			 * For some reason the initiator has closed the
			 * socket on us. This is most likely caused because
			 * of some network related condition
			 * (e.g. broken cable). We'll shutdown our side and
			 * wait for a reconnect from the initiator.
			 */
			queue_prt(c->c_mgmtq, Q_CONN_ERRS,
			    "CON%x  iscsi_pkt -- initiator closed socket",
			    c->c_num);
		} else {

			/*
			 * This is not good.
			 */
			queue_prt(c->c_mgmtq, Q_CONN_ERRS,
			    "CON%x  iscsi_pkt write failed, errno %d",
			    c->c_num, errno);
		}
		conn_state(c, T8);
		return;
	}

	/*
	 * Only start generating digest values once we've completed the
	 * login phase. If the state is not checked here and during login
	 * header or data digests have been enabled we would generate
	 * a digest value during the Login RSP PDU which the initiator
	 * is not expecting.
	 */
	if ((c->c_state == S5_LOGGED_IN) && (c->c_header_digest == True)) {
		crc = iscsi_crc32c((void *)h, sizeof (*h));
		if (write(c->c_fd, &crc, sizeof (crc)) != sizeof (crc)) {
			conn_state(c, T8);
			return;
		}
	}

	if (dlen) {
		if (write(c->c_fd, opt_text, dlen) != dlen) {
			conn_state(c, T8);
			return;
		}

		/*
		 * Find out how many pad bytes we need to send out.
		 */
		pad_len = (ISCSI_PAD_WORD_LEN -
		    (dlen & (ISCSI_PAD_WORD_LEN - 1))) &
		    (ISCSI_PAD_WORD_LEN - 1);
		if (pad_len) {
			if (write(c->c_fd, pad_text, pad_len) != pad_len) {
				conn_state(c, T8);
				return;
			}
		}

		if ((c->c_state == S5_LOGGED_IN) &&
		    (c->c_data_digest == True)) {

			crc = iscsi_crc32c((void *)opt_text,
			    (unsigned long)dlen);

			/*
			 * Include the pad information in the calculation of
			 * the CRC for the data.
			 */
			crc = iscsi_crc32c_continued((void *)pad_text,
			    (unsigned long)pad_len, crc);

			if (write(c->c_fd, &crc, sizeof (crc)) !=
			    sizeof (crc)) {
				conn_state(c, T8);
				return;
			}
		}
	}
#ifdef FULL_DEBUG
	if (dlen != 0) {
		queue_prt(c->c_mgmtq, Q_CONN_IO,
		    "CON%x  Response(0x%x), Data: len=%d addr=0x%llx",
		    c->c_num, h->opcode, dlen, opt_text);
	}
#endif
}
