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
#pragma ident	"@(#)listen.c	1.1	06/05/16 SMI"

/*
 * Each group has a listen thread. It is created at the time
 * of a group creation and destroyed when a group does not have
 * any console associated with it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread.h>
#include <assert.h>
#include <signal.h>
#include <ctype.h>
#include <syslog.h>
#include "vntsd.h"

/*
 * check the state of listen thread. exit if there is an fatal error
 * or the group is removed.
 */
static void
listen_chk_status(vntsd_group_t *groupp, int status)
{
	char	    err_msg[VNTSD_LINE_LEN];


	D1(stderr, "t@%d listen_chk_status() status=%d group=%s "
	    "tcp=%lld group status = %x\n", thr_self(), status,
	    groupp->group_name, groupp->tcp_port, groupp->status);

	(void) snprintf(err_msg, sizeof (err_msg),
	    "Group:%s TCP port %lld status %x",
	    groupp->group_name, groupp->tcp_port, groupp->status);


	switch (status) {

	case VNTSD_SUCCESS:
		return;

	case VNTSD_STATUS_INTR:
		assert(groupp->status & VNTSD_GROUP_SIG_WAIT);
		/* close listen socket */
		(void) mutex_lock(&groupp->lock);
		(void) close(groupp->sockfd);
		groupp->sockfd = -1;

		/* let group know */
		groupp->status &= ~VNTSD_GROUP_SIG_WAIT;
		(void) cond_signal(&groupp->cvp);

		(void) mutex_unlock(&groupp->lock);
		/* exit thread */
		thr_exit(0);
		break;

	case VNTSD_STATUS_ACCEPT_ERR:
		return;

	case VNTSD_STATUS_NO_CONS:
	default:
		/* fatal, exit thread */

		(void) mutex_lock(&groupp->lock);
		(void) close(groupp->sockfd);
		groupp->sockfd = -1;
		(void) mutex_unlock(&groupp->lock);
		vntsd_log(status, err_msg);
		vntsd_clean_group(groupp);

		thr_exit(0);
		break;
	}
}

/* allocate and initialize listening socket. */
static int
open_socket(int port_no, int *sockfd)
{

	struct	    sockaddr_in addr;
	int	    on;


	/* allocate a socket */
	*sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (*sockfd < 0) {
		if (errno == EINTR) {
			return (VNTSD_STATUS_INTR);
		}
		return (VNTSD_ERR_LISTEN_SOCKET);
	}

#ifdef DEBUG
	/* set reuse local socket address */
	on = 1;
	if (setsockopt(*sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof (on))) {
		return (VNTSD_ERR_LISTEN_OPTS);
	}
#endif

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = (vntsd_ip_addr()).s_addr;
	addr.sin_port = htons(port_no);

	/* bind socket */
	if (bind(*sockfd, (struct sockaddr *)&addr, sizeof (addr)) < 0) {
		if (errno == EINTR) {
			return (VNTSD_STATUS_INTR);
		}
		return (VNTSD_ERR_LISTEN_BIND);

	}

	if (listen(*sockfd, VNTSD_MAX_SOCKETS) == -1) {
		if (errno == EINTR) {
			return (VNTSD_STATUS_INTR);
		}
		return (VNTSD_ERR_LISTEN_BIND);
	}

	D1(stderr, "t@%d open_socket() sockfd=%d\n", thr_self(), *sockfd);
	return (VNTSD_SUCCESS);
}

/* ceate console selection thread */
static int
create_console_thread(vntsd_group_t *groupp, int sockfd)
{
	vntsd_client_t	    *clientp;
	vntsd_thr_arg_t	    arg;
	int		    rv;


	assert(groupp);
	D1(stderr, "t@%d create_console_thread@%lld:client@%d\n", thr_self(),
	    groupp->tcp_port, sockfd);

	/* allocate a new client */
	clientp = (vntsd_client_t *)malloc(sizeof (vntsd_client_t));
	if (clientp  == NULL) {
		return (VNTSD_ERR_NO_MEM);
	}

	/* initialize the client */
	bzero(clientp, sizeof (vntsd_client_t));

	clientp->sockfd = sockfd;
	clientp->cons_tid = (thread_t)-1;

	(void) mutex_init(&clientp->lock, USYNC_THREAD|LOCK_ERRORCHECK, NULL);

	/* append client to group */
	(void) mutex_lock(&groupp->lock);

	if ((rv = vntsd_que_append(&groupp->no_cons_clientpq, clientp))
	    != VNTSD_SUCCESS) {
		(void) mutex_unlock(&groupp->lock);
		vntsd_free_client(clientp);
		return (rv);
	}

	(void) mutex_unlock(&groupp->lock);

	(void) mutex_lock(&clientp->lock);

	/* parameters for console thread */
	bzero(&arg, sizeof (arg));

	arg.handle = groupp;
	arg.arg = clientp;

	/* create console selection thread */
	if (thr_create(NULL, 0, (thr_func_t)vntsd_console_thread,
		    &arg, THR_DETACHED, &clientp->cons_tid)) {

		(void) mutex_unlock(&clientp->lock);
		(void) mutex_lock(&groupp->lock);
		(void) vntsd_que_rm(&groupp->no_cons_clientpq, clientp);
		(void) mutex_unlock(&groupp->lock);
		vntsd_free_client(clientp);

		return (VNTSD_ERR_CREATE_CONS_THR);
	}

	(void) mutex_unlock(&clientp->lock);

	return (VNTSD_SUCCESS);
}

/* listen thread */
void *
vntsd_listen_thread(vntsd_group_t *groupp)
{

	int		newsockfd;
	size_t		clilen;
	struct		sockaddr_in cli_addr;
	int		rv;
	int		num_cons;

	assert(groupp);

	D1(stderr, "t@%d listen@%lld\n", thr_self(), groupp->tcp_port);


	/* initialize listen socket */
	(void) mutex_lock(&groupp->lock);
	rv = open_socket(groupp->tcp_port, &groupp->sockfd);
	(void) mutex_unlock(&groupp->lock);
	listen_chk_status(groupp, rv);

	for (; ; ) {

		clilen = sizeof (cli_addr);

		/* listen to the socket */
		newsockfd = accept(groupp->sockfd, (struct sockaddr *)&cli_addr,
			    &clilen);

		D1(stderr, "t@%d listen_thread() connected sockfd=%d\n",
		    thr_self(), newsockfd);

		if (newsockfd <=  0) {

			if (errno == EINTR) {
				listen_chk_status(groupp, VNTSD_STATUS_INTR);
			} else {
				listen_chk_status(groupp,
				    VNTSD_STATUS_ACCEPT_ERR);
			}
			continue;
		}
		num_cons = vntsd_chk_group_total_cons(groupp);
		if (num_cons == 0) {
			(void) close(newsockfd);
			listen_chk_status(groupp, VNTSD_STATUS_NO_CONS);
		}

		/* a connection is established */
		rv = vntsd_set_telnet_options(newsockfd);
		if (rv != VNTSD_SUCCESS) {
			(void) close(newsockfd);
			listen_chk_status(groupp, rv);
		}
		rv = create_console_thread(groupp, newsockfd);
		if (rv != VNTSD_SUCCESS) {
			(void) close(newsockfd);
			listen_chk_status(groupp, rv);
		}
	}

	/*NOTREACHED*/
	return (NULL);
}
