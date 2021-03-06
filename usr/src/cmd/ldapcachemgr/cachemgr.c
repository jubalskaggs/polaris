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

#pragma ident	"@(#)cachemgr.c	1.15	05/10/11 SMI"

/*
 * Simple doors ldap cache daemon
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <door.h>
#include <time.h>
#include <string.h>
#include <libintl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <thread.h>
#include <stdarg.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <memory.h>
#include <sys/types.h>
#include <syslog.h>
#include <locale.h>	/* LC_ALL */
#include "cachemgr.h"

static void	detachfromtty();
admin_t		current_admin;
static int	will_become_server;

static void switcher(void *cookie, char *argp, size_t arg_size,
			door_desc_t *dp, uint_t n_desc);
static void usage(char *s);
static int cachemgr_set_lf(admin_t *ptr, char *logfile);
static int client_getadmin(admin_t *ptr);
static int getadmin(ldap_return_t *out);
static int setadmin(ldap_return_t *out, ldap_call_t *ptr);
static  int client_setadmin(admin_t *ptr);
static int client_showstats(admin_t *ptr);

#ifdef SLP
int			use_slp = 0;
static unsigned int	refresh = 10800;	/* dynamic discovery interval */
#endif /* SLP */

static ldap_stat_t *
getcacheptr(char *s)
{
	static const char *caches[1] = {"ldap"};

	if (strncmp(caches[0], s, strlen(caches[0])) == 0)
		return (&current_admin.ldap_stat);

	return (NULL);
}

char *
getcacheopt(char *s)
{
	while (*s && *s != ',')
		s++;
	return ((*s == ',') ? (s + 1) : NULL);
}

/*
 *  This is here to prevent the ldap_cachemgr becomes
 *  daemonlized to early to soon during boot time.
 *  This causes problems during boot when automounter
 *  and others try to use libsldap before ldap_cachemgr
 *  finishes walking the server list.
 */
static void
sig_ok_to_exit(int signo)
{
	if (signo == SIGUSR1) {
		logit("sig_ok_to_exit(): parent exiting...\n");
		exit(0);
	} else {
		logit("sig_ok_to_exit(): invalid signal(%d) received.\n",
			signo);
		syslog(LOG_ERR, gettext("ldap_cachemgr: "
			"invalid signal(%d) received."), signo);
		exit(1);
	}
}
#define	LDAP_TABLES		1	/* ldap */
#define	TABLE_THREADS		10
#define	COMMON_THREADS		20
#define	CACHE_MISS_THREADS	(COMMON_THREADS + LDAP_TABLES * TABLE_THREADS)
#define	CACHE_HIT_THREADS	20
#define	MAX_SERVER_THREADS	(CACHE_HIT_THREADS + CACHE_MISS_THREADS)

static sema_t common_sema;
static sema_t ldap_sema;
static thread_key_t lookup_state_key;

static void
initialize_lookup_clearance()
{
	(void) thr_keycreate(&lookup_state_key, NULL);
	(void) sema_init(&common_sema, COMMON_THREADS, USYNC_THREAD, 0);
	(void) sema_init(&ldap_sema, TABLE_THREADS, USYNC_THREAD, 0);
}

int
get_clearance(int callnumber)
{
	sema_t	*table_sema = NULL;
	char	*tab;

	if (sema_trywait(&common_sema) == 0) {
		(void) thr_setspecific(lookup_state_key, NULL);
		return (0);
	}

	switch (callnumber) {
		case GETLDAPCONFIG:
			tab = "ldap";
			table_sema = &ldap_sema;
			break;
		default:
			logit("Internal Error: get_clearance\n");
			break;
	}

	if (sema_trywait(table_sema) == 0) {
		(void) thr_setspecific(lookup_state_key, (void*)1);
		return (0);
	}

	if (current_admin.debug_level >= DBG_CANT_FIND) {
		logit("get_clearance: throttling load for %s table\n", tab);
	}

	return (-1);
}

int
release_clearance(int callnumber)
{
	int	which;
	sema_t	*table_sema = NULL;

	(void) thr_getspecific(lookup_state_key, (void**)&which);
	if (which == 0) /* from common pool */ {
		(void) sema_post(&common_sema);
		return (0);
	}

	switch (callnumber) {
		case GETLDAPCONFIG:
			table_sema = &ldap_sema;
			break;
		default:
			logit("Internal Error: release_clearance\n");
			break;
	}
	(void) sema_post(table_sema);

	return (0);
}


static mutex_t		create_lock;
static int		num_servers = 0;
static thread_key_t	server_key;


/*
 * Bind a TSD value to a server thread. This enables the destructor to
 * be called if/when this thread exits.  This would be a programming error,
 * but better safe than sorry.
 */

/*ARGSUSED*/
static void *
server_tsd_bind(void *arg)
{
	static void	*value = 0;

	/*
	 * disable cancellation to prevent hangs when server
	 * threads disappear
	 */

	(void) thr_setspecific(server_key, value);
	door_return(NULL, 0, NULL, 0);

	return (value);
}

/*
 * Server threads are created here.
 */

/*ARGSUSED*/
static void
server_create(door_info_t *dip)
{
	(void) mutex_lock(&create_lock);
	if (++num_servers > MAX_SERVER_THREADS) {
		num_servers--;
		(void) mutex_unlock(&create_lock);
		return;
	}
	(void) mutex_unlock(&create_lock);
	(void) thr_create(NULL, 0, server_tsd_bind, NULL,
		THR_BOUND|THR_DETACHED, NULL);
}

/*
 * Server thread are destroyed here
 */

/*ARGSUSED*/
static void
server_destroy(void *arg)
{
	(void) mutex_lock(&create_lock);
	num_servers--;
	(void) mutex_unlock(&create_lock);
}

int
main(int argc, char ** argv)
{
	int			did;
	int			opt;
	int			errflg = 0;
	int			showstats = 0;
	int			doset = 0;
	int			dofg = 0;
	struct stat		buf;
	sigset_t		myset;
	struct sigaction	sighupaction;
	static void		client_killserver();
	int			debug_level = 0;

	/* setup for localization */
	(void) setlocale(LC_ALL, "");
	(void) textdomain(TEXT_DOMAIN);

	openlog("ldap_cachemgr", LOG_PID, LOG_DAEMON);

	if (chdir(NSLDAPDIRECTORY) < 0) {
		(void) fprintf(stderr, gettext("chdir(\"%s\") failed: %s\n"),
			NSLDAPDIRECTORY, strerror(errno));
		exit(1);
	}

	/*
	 * Correctly set file mode creation mask, so to make the new files
	 * created for door calls being readable by all.
	 */
	(void) umask(0);

	/*
	 *  Special case non-root user here -	he/she/they/it can just print
	 *					stats
	 */

	if (geteuid()) {
		if (argc != 2 || strcmp(argv[1], "-g")) {
			(void) fprintf(stderr,
			    gettext("Must be root to use any option "
			    "other than -g.\n\n"));
			usage(argv[0]);
		}

		if ((__ns_ldap_cache_ping() != SUCCESS) ||
		    (client_getadmin(&current_admin) != 0)) {
			(void) fprintf(stderr,
				gettext("%s doesn't appear to be running.\n"),
				argv[0]);
			exit(1);
		}
		(void) client_showstats(&current_admin);
		exit(0);
	}



	/*
	 *  Determine if there is already a daemon running
	 */

	will_become_server = (__ns_ldap_cache_ping() != SUCCESS);

	/*
	 *  load normal config file
	 */

	if (will_become_server) {
		static const ldap_stat_t defaults = {
			0,		/* stat */
			DEFAULTTTL};	/* ttl */

		current_admin.ldap_stat = defaults;
		(void) strcpy(current_admin.logfile, LOGFILE);
	} else {
		if (client_getadmin(&current_admin)) {
			(void) fprintf(stderr, gettext("Cannot contact %s "
				"properly(?)\n"), argv[0]);
			exit(1);
		}
	}

#ifndef SLP
	while ((opt = getopt(argc, argv, "fKgl:r:d:")) != EOF) {
#else
	while ((opt = getopt(argc, argv, "fKgs:l:r:d:")) != EOF) {
#endif /* SLP */
		ldap_stat_t	*cache;

		switch (opt) {
		case 'K':
			client_killserver();
			exit(0);
			break;
		case 'g':
			showstats++;
			break;
		case 'f':
			dofg++;
			break;
		case 'r':
			doset++;
			cache = getcacheptr("ldap");
			if (!optarg) {
				errflg++;
				break;
			}
			cache->ldap_ttl = atoi(optarg);
			break;
		case 'l':
			doset++;
			(void) strlcpy(current_admin.logfile,
				optarg, sizeof (current_admin.logfile));
			break;
		case 'd':
			doset++;
			debug_level = atoi(optarg);
			break;
#ifdef SLP
		case 's':	/* undocumented: use dynamic (SLP) config */
			use_slp = 1;
			break;
#endif /* SLP */
		default:
			errflg++;
			break;
		}
	}

	if (errflg)
	    usage(argv[0]);

	/*
	 * will not show statistics if no daemon running
	 */
	if (will_become_server && showstats) {
		(void) fprintf(stderr,
			gettext("%s doesn't appear to be running.\n"),
				argv[0]);
		exit(1);
	}

	if (!will_become_server) {
		if (showstats) {
			(void) client_showstats(&current_admin);
		}
		if (doset) {
			current_admin.debug_level = debug_level;
			if (client_setadmin(&current_admin) < 0) {
				(void) fprintf(stderr,
					gettext("Error during admin call\n"));
				exit(1);
			}
		}
		if (!showstats && !doset) {
			(void) fprintf(stderr,
			gettext("%s already running....use '%s "
				"-K' to stop\n"), argv[0], argv[0]);
		}
		exit(0);
	}

	/*
	 *   daemon from here on
	 */

	if (debug_level) {
		/*
		 * we're debugging...
		 */
		if (strlen(current_admin.logfile) == 0)
			/*
			 * no specified log file
			 */
			(void) strcpy(current_admin.logfile, LOGFILE);
		else
			(void) cachemgr_set_lf(&current_admin,
				current_admin.logfile);
		/*
		 * validate the range of debug level number
		 * and set the number to current_admin.debug_level
		 */
		if (cachemgr_set_dl(&current_admin, debug_level) < 0) {
				/*
				 * print error messages to the screen
				 * cachemgr_set_dl prints msgs to cachemgr.log
				 * only
				 */
				(void) fprintf(stderr,
				gettext("Incorrect Debug Level: %d\n"
				"It should be between %d and %d\n"),
				debug_level, DBG_OFF, MAXDEBUG);
			exit(-1);
		}
	} else {
		if (strlen(current_admin.logfile) == 0)
			(void) strcpy(current_admin.logfile, "/dev/null");
			(void) cachemgr_set_lf(&current_admin,
				current_admin.logfile);
	}

	if (dofg == 0)
		detachfromtty(argv[0]);

	/*
	 * perform some initialization
	 */

	initialize_lookup_clearance();

	if (getldap_init() != 0)
		exit(-1);

	/*
	 * Establish our own server thread pool
	 */

	door_server_create(server_create);
	if (thr_keycreate(&server_key, server_destroy) != 0) {
		logit("thr_keycreate() call failed\n");
		syslog(LOG_ERR,
			gettext("ldap_cachemgr: thr_keycreate() call failed"));
		perror("thr_keycreate");
		exit(-1);
	}

	/*
	 * Create a door
	 */

	if ((did = door_create(switcher, LDAP_CACHE_DOOR_COOKIE,
	    DOOR_UNREF | DOOR_REFUSE_DESC | DOOR_NO_CANCEL)) < 0) {
		logit("door_create() call failed\n");
		syslog(LOG_ERR, gettext(
			"ldap_cachemgr: door_create() call failed"));
		perror("door_create");
		exit(-1);
	}

	/*
	 * bind to file system
	 */

	if (stat(LDAP_CACHE_DOOR, &buf) < 0) {
		int	newfd;

		if ((newfd = creat(LDAP_CACHE_DOOR, 0444)) < 0) {
			logit("Cannot create %s:%s\n",
				LDAP_CACHE_DOOR,
				strerror(errno));
			exit(1);
		}
		(void) close(newfd);
	}

	if (fattach(did, LDAP_CACHE_DOOR) < 0) {
		if ((errno != EBUSY) ||
		    (fdetach(LDAP_CACHE_DOOR) <  0) ||
		    (fattach(did, LDAP_CACHE_DOOR) < 0)) {
			logit("fattach() call failed\n");
			syslog(LOG_ERR, gettext(
				"ldap_cachemgr: fattach() call failed"));
			perror("fattach");
			exit(2);
		}
	}

	/* catch SIGHUP revalid signals */
	sighupaction.sa_handler = getldap_revalidate;
	sighupaction.sa_flags = 0;
	(void) sigemptyset(&sighupaction.sa_mask);
	(void) sigemptyset(&myset);
	(void) sigaddset(&myset, SIGHUP);

	if (sigaction(SIGHUP, &sighupaction, NULL) < 0) {
		logit("sigaction() call failed\n");
		syslog(LOG_ERR,
			gettext("ldap_cachemgr: sigaction() call failed"));
		perror("sigaction");
		exit(1);
	}

	if (thr_sigsetmask(SIG_BLOCK, &myset, NULL) < 0) {
		logit("thr_sigsetmask() call failed\n");
		syslog(LOG_ERR,
			gettext("ldap_cachemgr: thr_sigsetmask() call failed"));
		perror("thr_sigsetmask");
		exit(1);
	}

	/*
	 *  kick off revalidate threads only if ttl != 0
	 */

	if (thr_create(NULL, NULL, (void *(*)(void*))getldap_refresh,
		0, 0, NULL) != 0) {
		logit("thr_create() call failed\n");
		syslog(LOG_ERR,
			gettext("ldap_cachemgr: thr_create() call failed"));
		perror("thr_create");
		exit(1);
	}

	/*
	 *  kick off the thread which refreshes the server info
	 */

	if (thr_create(NULL, NULL, (void *(*)(void*))getldap_serverInfo_refresh,
		0, 0, NULL) != 0) {
		logit("thr_create() call failed\n");
		syslog(LOG_ERR,
			gettext("ldap_cachemgr: thr_create() call failed"));
		perror("thr_create");
		exit(1);
	}

#ifdef SLP
	if (use_slp) {
		/* kick off SLP discovery thread */
		if (thr_create(NULL, NULL, (void *(*)(void *))discover,
			(void *)&refresh, 0, NULL) != 0) {
			logit("thr_create() call failed\n");
			syslog(LOG_ERR, gettext("ldap_cachemgr: thr_create() "
				"call failed"));
			perror("thr_create");
			exit(1);
		}
	}
#endif /* SLP */

	if (thr_sigsetmask(SIG_UNBLOCK, &myset, NULL) < 0) {
		logit("thr_sigsetmask() call failed\n");
		syslog(LOG_ERR,
			gettext("ldap_cachemgr: the_sigsetmask() call failed"));
		perror("thr_sigsetmask");
		exit(1);
	}

	/*CONSTCOND*/
	while (1) {
		(void) pause();
	}
}


/*ARGSUSED*/
static void
switcher(void *cookie, char *argp, size_t arg_size,
    door_desc_t *dp, uint_t n_desc)
{
	dataunion		u;
	ldap_call_t	*ptr = (ldap_call_t *)argp;
	door_cred_t	dc;

	if (argp == DOOR_UNREF_DATA) {
		logit("Door Slam... invalid door param\n");
		syslog(LOG_ERR, gettext("ldap_cachemgr: Door Slam... "
			"invalid door param"));
		(void) printf(gettext("Door Slam... invalid door param\n"));
		exit(0);
	}

	if (ptr == NULL) { /* empty door call */
		(void) door_return(NULL, 0, 0, 0); /* return the favor */
	}

	switch (ptr->ldap_callnumber) {
	case NULLCALL:
		u.data.ldap_ret.ldap_return_code = SUCCESS;
		u.data.ldap_ret.ldap_bufferbytesused = sizeof (ldap_return_t);
		break;
	case GETLDAPCONFIG:
		getldap_lookup(&u.data.ldap_ret, ptr);
		current_admin.ldap_stat.ldap_numbercalls++;
		break;
	case GETADMIN:
		(void) getadmin(&u.data.ldap_ret);
		break;
	case SETADMIN:
	case KILLSERVER:
		if (door_cred(&dc) < 0) {
			logit("door_cred() call failed\n");
			syslog(LOG_ERR, gettext("ldap_cachemgr: door_cred() "
				"call failed"));
			perror("door_cred");
			break;
		}
		if (dc.dc_euid != 0 && ptr->ldap_callnumber == SETADMIN) {
			logit("SETADMIN call failed (cred): caller "
			    "pid %ld, uid %ld, euid %ld\n",
			    dc.dc_pid, dc.dc_ruid, dc.dc_euid);
			u.data.ldap_ret.ldap_return_code = NOTFOUND;
			break;
		}
		if (ptr->ldap_callnumber == KILLSERVER) {
			logit("ldap_cachemgr received KILLSERVER cmd from "
			    "pid %ld, uid %ld, euid %ld\n",
			    dc.dc_pid, dc.dc_ruid, dc.dc_euid);
			exit(0);
		} else {
			(void) setadmin(&u.data.ldap_ret, ptr);
		}
		break;
	case GETLDAPSERVER:
		getldap_getserver(&u.data.ldap_ret, ptr);
		current_admin.ldap_stat.ldap_numbercalls++;
		break;
	case GETCACHE:
		getldap_get_cacheData(&u.data.ldap_ret, ptr);
		current_admin.ldap_stat.ldap_numbercalls++;
		break;
	case SETCACHE:
		getldap_set_cacheData(&u.data.ldap_ret, ptr);
		current_admin.ldap_stat.ldap_numbercalls++;
		break;
	case GETCACHESTAT:
		getldap_get_cacheStat(&u.data.ldap_ret);
		current_admin.ldap_stat.ldap_numbercalls++;
		break;
	default:
		logit("Unknown ldap service door call op %d\n",
		    ptr->ldap_callnumber);
		u.data.ldap_ret.ldap_return_code = -99;
		u.data.ldap_ret.ldap_bufferbytesused = sizeof (ldap_return_t);
		break;
	}
	door_return((char *)&u.data,
		u.data.ldap_ret.ldap_bufferbytesused, NULL, 0);
}

static void
usage(char *s)
{
	(void) fprintf(stderr,
		gettext("Usage: %s [-d debug_level] [-l logfilename]\n"), s);
	(void) fprintf(stderr, gettext("	[-K] "
					"[-r revalidate_interval] "));
#ifndef SLP
	(void) fprintf(stderr, gettext("	[-g]\n"));
#else
	(void) fprintf(stderr, gettext("	[-g] [-s]\n"));
#endif /* SLP */
	exit(1);
}


static int	logfd = -1;

static int
cachemgr_set_lf(admin_t *ptr, char *logfile)
{
	int	newlogfd;

	/*
	 *  we don't really want to try and open the log file
	 *  /dev/null since that will fail w/ our security fixes
	 */

	if (logfile == NULL || *logfile == 0) {
		/*EMPTY*/;
	} else if (strcmp(logfile, "/dev/null") == 0) {
		(void) strcpy(current_admin.logfile, "/dev/null");
		(void) close(logfd);
		logfd = -1;
	} else {
		if ((newlogfd =
			open(logfile, O_EXCL|O_WRONLY|O_CREAT, 0644)) < 0) {
			/*
			 * File already exists... now we need to get cute
			 * since opening a file in a world-writeable directory
			 * safely is hard = it could be a hard link or a
			 * symbolic link to a system file.
			 *
			 */
			struct stat	before;

			if (lstat(logfile, &before) < 0) {
				logit("Cannot open new logfile \"%s\": %sn",
					logfile, strerror(errno));
				return (-1);
			}
			if (S_ISREG(before.st_mode) &&	/* no symbolic links */
			    (before.st_nlink == 1) &&	/* no hard links */
			    (before.st_uid == 0)) {	/* owned by root */
				if ((newlogfd =
				    open(logfile,
				    O_APPEND|O_WRONLY, 0644)) < 0) {
					logit("Cannot open new logfile "
						"\"%s\": %s\n",
						logfile, strerror(errno));
					return (-1);
				}
			} else {
				logit("Cannot use specified logfile "
				    "\"%s\": file is/has links or isn't "
				    "owned by root\n", logfile);
				return (-1);
			}
		}
		(void) strlcpy(ptr->logfile, logfile, sizeof (ptr->logfile));
		(void) close(logfd);
		logfd = newlogfd;
		logit("Starting ldap_cachemgr, logfile %s\n", logfile);
	}
	return (0);
}

/*PRINTFLIKE1*/
void
logit(char *format, ...)
{
	static mutex_t	loglock;
	struct timeval	tv;
	char		buffer[BUFSIZ];
	va_list		ap;

	va_start(ap, format);

	if (logfd >= 0) {
		int	safechars;

		(void) gettimeofday(&tv, NULL);
		(void) ctime_r(&tv.tv_sec, buffer, BUFSIZ);
		(void) snprintf(buffer+19, BUFSIZE, ".%.4ld	",
			tv.tv_usec/100);
		safechars = sizeof (buffer) - 30;
		if (vsnprintf(buffer+25, safechars, format, ap) > safechars)
			(void) strcat(buffer, "...\n");
		(void) mutex_lock(&loglock);
		(void) write(logfd, buffer, strlen(buffer));
		(void) mutex_unlock(&loglock);
	}
	va_end(ap);
}


void
do_update(ldap_call_t *in)
{
	dataunion		u;

	switch (in->ldap_callnumber) {
	case GETLDAPCONFIG:
		getldap_lookup(&u.data.ldap_ret, in);
		break;
	default:
		assert(0);
		break;
	}

	free(in);
}


static int
client_getadmin(admin_t *ptr)
{
	dataunion		u;
	ldap_data_t	*dptr;
	int		ndata;
	int		adata;

	u.data.ldap_call.ldap_callnumber = GETADMIN;
	ndata = sizeof (u);
	adata = sizeof (u.data);
	dptr = &u.data;

	if (__ns_ldap_trydoorcall(&dptr, &ndata, &adata) != SUCCESS) {
		return (-1);
	}
	(void) memcpy(ptr, dptr->ldap_ret.ldap_u.buff, sizeof (*ptr));

	return (0);
}

static int
getadmin(ldap_return_t *out)
{
	out->ldap_return_code = SUCCESS;
	out->ldap_bufferbytesused = sizeof (current_admin);
	(void) memcpy(out->ldap_u.buff, &current_admin, sizeof (current_admin));

	return (0);
}


static int
setadmin(ldap_return_t *out, ldap_call_t *ptr)
{
	admin_t	*new;

	out->ldap_return_code = SUCCESS;
	out->ldap_bufferbytesused = sizeof (ldap_return_t);
	new = (admin_t *)ptr->ldap_u.domainname;

	/*
	 *  global admin stuff
	 */

	if ((cachemgr_set_lf(&current_admin, new->logfile) < 0) ||
	    cachemgr_set_dl(&current_admin, new->debug_level) < 0) {
		out->ldap_return_code = NOTFOUND;
		return (-1);
	}

	if (cachemgr_set_ttl(&current_admin.ldap_stat,
			"ldap",
			new->ldap_stat.ldap_ttl) < 0) {
		out->ldap_return_code = NOTFOUND;
		return (-1);
	}
	out->ldap_return_code = SUCCESS;

	return (0);
}


static void
client_killserver()
{
	dataunion		u;
	ldap_data_t		*dptr;
	int			ndata;
	int			adata;

	u.data.ldap_call.ldap_callnumber = KILLSERVER;
	ndata = sizeof (u);
	adata = sizeof (ldap_call_t);
	dptr = &u.data;

	__ns_ldap_trydoorcall(&dptr, &ndata, &adata);
}


static int
client_setadmin(admin_t *ptr)
{
	dataunion		u;
	ldap_data_t		*dptr;
	int			ndata;
	int			adata;

	u.data.ldap_call.ldap_callnumber = SETADMIN;
	(void) memcpy(u.data.ldap_call.ldap_u.domainname, ptr, sizeof (*ptr));
	ndata = sizeof (u);
	adata = sizeof (*ptr);
	dptr = &u.data;

	if (__ns_ldap_trydoorcall(&dptr, &ndata, &adata) != SUCCESS) {
		return (-1);
	}

	return (0);
}

static int
client_showstats(admin_t *ptr)
{
	dataunion	u;
	ldap_data_t	*dptr;
	int		ndata;
	int		adata;
	char		*rbuf, *sptr, *rest;

	/*
	 * print admin data
	 */
	(void) printf(gettext("\ncachemgr configuration:\n"));
	(void) printf(gettext("server debug level %10d\n"), ptr->debug_level);
	(void) printf(gettext("server log file\t\"%s\"\n"), ptr->logfile);
	(void) printf(gettext("number of calls to ldapcachemgr %10d\n"),
		ptr->ldap_stat.ldap_numbercalls);

	/*
	 * get cache data statistics
	 */
	u.data.ldap_call.ldap_callnumber = GETCACHESTAT;
	ndata = sizeof (u);
	adata = sizeof (ldap_call_t);
	dptr = &u.data;

	if (__ns_ldap_trydoorcall(&dptr, &ndata, &adata) != SUCCESS) {
		(void) printf(
			gettext("\nCache data statistics not available!\n"));
		return (0);
	}

	/*
	 * print cache data statistics line by line
	 */
	(void) printf(gettext("\ncachemgr cache data statistics:\n"));
	rbuf = dptr->ldap_ret.ldap_u.buff;
	sptr = strtok_r(rbuf, DOORLINESEP, &rest);
	for (;;) {
		(void) printf("%s\n", sptr);
		sptr = strtok_r(NULL, DOORLINESEP, &rest);
		if (sptr == NULL)
			break;
	}
	return (0);
}


/*
 * detach from tty
 */
static void
detachfromtty(char *pgm)
{
	int 	status;
	pid_t	pid, wret;

	(void) close(0);
	(void) close(1);
	/*
	 * Block the SIGUSR1 signal
	 * just in case that the child
	 * process may run faster than
	 * the parent process and
	 * send this signal before
	 * the signal handler is ready
	 * in the parent process.
	 * This error will cause the parent
	 * to exit with the User Signal 1
	 * exit code (144).
	 */
	(void) sighold(SIGUSR1);
	pid = fork1();
	switch (pid) {
		case (pid_t)-1:
			logit("detachfromtty(): fork1() call failed\n");
			(void) fprintf(stderr,
					gettext("%s: fork1() call failed.\n"),
					pgm);
			syslog(LOG_ERR,
				gettext("ldap_cachemgr: fork1() call failed."));
			exit(1);
			break;
		case 0:
			/*
			 * child process does not
			 * need to worry about
			 * the SIGUSR1 signal
			 */
			(void) sigrelse(SIGUSR1);
			(void) close(2);
			break;
		default:
			/*
			 * Wait forever until the child process
			 * has exited, or has signalled that at
			 * least one server in the server list
			 * is up.
			 */
			if (signal(SIGUSR1, sig_ok_to_exit) == SIG_ERR) {
				logit("detachfromtty(): "
					"can't set up signal handler to "
					" catch SIGUSR1.\n");
				(void) fprintf(stderr,
					gettext("%s: signal() call failed.\n"),
					pgm);
				syslog(LOG_ERR, gettext("ldap_cachemgr: "
					"can't set up signal handler to "
					" catch SIGUSR1."));
				exit(1);
			}

			/*
			 * now unblock the SIGUSR1 signal
			 * to handle the pending or
			 * soon to arrive SIGUSR1 signal
			 */
			(void) sigrelse(SIGUSR1);
			wret = waitpid(pid, &status, 0);

			if (wret == -1) {
				logit("detachfromtty(): "
					"waitpid() call failed\n");
				(void) fprintf(stderr,
					gettext("%s: waitpid() call failed.\n"),
					pgm);
				syslog(LOG_ERR,
					gettext("ldap_cachemgr: waitpid() "
						"call failed."));
				exit(1);
			}
			if (wret != pid) {
				logit("detachfromtty(): "
					"waitpid() returned %ld when "
					"child pid was %ld\n",
					wret, pid);
				(void) fprintf(stderr,
					gettext(
					"%s: waitpid() returned %ld when "
					"child pid was %ld.\n"),
					pgm, wret, pid);
				syslog(LOG_ERR,
					gettext("ldap_cachemgr: waitpid() "
						"returned different "
						"child pid."));
				exit(1);
			}

			/* evaluate return status */
			if (WIFEXITED(status)) {
				if (WEXITSTATUS(status) == 0) {
					exit(0);
				}
				logit("detachfromtty(): "
					"child failed (rc = %d).\n",
					WEXITSTATUS(status));
				(void) fprintf(stderr,
					gettext("%s: failed. Please see "
					"syslog for details.\n"),
					pgm);
				syslog(LOG_ERR,
					gettext("ldap_cachemgr: failed "
					"(rc = %d)."),
					WEXITSTATUS(status));
			} else if (WIFSIGNALED(status)) {
				logit("detachfromtty(): "
					"child terminated by signal %d.\n",
					WTERMSIG(status));
				(void) fprintf(stderr,
				gettext("%s: terminated by signal %d.\n"),
					pgm, WTERMSIG(status));
				syslog(LOG_ERR,
					gettext("ldap_cachemgr: terminated by "
					"signal %d.\n"),
					WTERMSIG(status));
			} else if (WCOREDUMP(status)) {
				logit("detachfromtty(): child core dumped.\n"),
				(void) fprintf(stderr,
					gettext("%s: core dumped.\n"),
					pgm);
				syslog(LOG_ERR,
					gettext("ldap_cachemgr: "
						"core dumped.\n"));
			}

			exit(1);
	}
	(void) setsid();
	if (open("/dev/null", O_RDWR, 0) != -1) {
		(void) dup(0);
		(void) dup(0);
	}
}
