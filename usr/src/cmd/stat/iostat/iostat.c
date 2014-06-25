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
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * rewritten from UCB 4.13 83/09/25
 * rewritten from SunOS 4.1 SID 1.18 89/10/06
 */

#pragma ident	"@(#)iostat.c	1.50	05/07/20 SMI"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <memory.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <sys/sysinfo.h>
#include <inttypes.h>
#include <strings.h>
#include <sys/systeminfo.h>
#include <kstat.h>

#include "dsr.h"
#include "statcommon.h"

#define	DISK_OLD		0x0001
#define	DISK_NEW		0x0002
#define	DISK_EXTENDED		0x0004
#define	DISK_ERRORS		0x0008
#define	DISK_EXTENDED_ERRORS	0x0010
#define	DISK_IOPATH		0x0020
#define	DISK_NORMAL		(DISK_OLD | DISK_NEW)

#define	DISK_IO_MASK		(DISK_OLD | DISK_NEW | DISK_EXTENDED)
#define	DISK_ERROR_MASK		(DISK_ERRORS | DISK_EXTENDED_ERRORS)
#define	PRINT_VERTICAL		(DISK_ERROR_MASK | DISK_EXTENDED | DISK_IOPATH)

#define	REPRINT 19

/*
 * It's really a pseudo-gigabyte. We use 1000000000 bytes so that the disk
 * labels don't look bad. 1GB is really 1073741824 bytes.
 */
#define	DISK_GIGABYTE   1000000000.0

/*
 * Function desciptor to be called when extended
 * headers are used.
 */
typedef struct formatter {
	void (*nfunc)(void);
	struct formatter *next;
} format_t;

/*
 * Used to get formatting right when printing tty/cpu
 * data to the right of disk data
 */
enum show_disk_mode {
	SHOW_FIRST_ONLY,
	SHOW_SECOND_ONWARDS,
	SHOW_ALL
};

enum show_disk_mode show_disk_mode = SHOW_ALL;

char cmdname[] = "iostat";

static char one_blank[] = " ";
static char two_blanks[] = "  ";

/*
 * count for number of lines to be emitted before a header is
 * shown again. Only used for the basic format.
 */
static	uint_t	tohdr = 1;

/*
 * If we're in raw format, have we printed a header? We only do it
 * once for raw but we emit it every REPRINT lines in non-raw format.
 * This applies only for the basic header. The extended header is
 * done only once in both formats.
 */
static	uint_t	hdr_out;

/*
 * Flags representing arguments from command line
 */
static	uint_t	do_tty;			/* show tty info (-t) */
static	uint_t	do_disk;		/* show disk info per selected */
					/* format (-d, -D, -e, -E, -x -X) */
static	uint_t	do_cpu;			/* show cpu info (-c) */
static	uint_t	do_interval;		/* do intervals (-I) */
static	int	do_partitions;		/* per-partition stats (-p) */
static	int	do_partitions_only;	/* per-partition stats only (-P) */
					/* no per-device stats for disks */
static	uint_t	do_conversions;		/* display disks as cXtYdZ (-n) */
static	uint_t	do_megabytes;		/* display data in MB/sec (-M) */
static  uint_t	do_controller;		/* display controller info (-C) */
static  uint_t	do_raw;			/* emit raw format (-r) */
static  uint_t	do_timestamp;		/* timestamp  each display (-T) */
static	uint_t	do_devid;		/* -E should show devid */

/*
 * Definition of allowable types of timestamps
 */
#define	CDATE 1
#define	UDATE 2

/*
 * Default number of disk drives to be displayed in basic format
 */
#define	DEFAULT_LIMIT	4

struct iodev_filter df;

static  uint_t	suppress_state;		/* skip state change messages */
static	uint_t	suppress_zero;		/* skip zero valued lines */
static  uint_t	show_mountpts;		/* show mount points */
static	int 	interval;		/* interval (seconds) to output */
static	int 	iter;			/* iterations from command line */

#define	SMALL_SCRATCH_BUFLEN	64
#define	DISPLAYED_NAME_FORMAT "%-9.9s"

static	char	disk_header[132];
static	uint_t 	dh_len;			/* disk header length for centering */
static  int 	lineout;		/* data waiting to be printed? */

static struct snapshot *newss;
static struct snapshot *oldss;
static	double	getime;		/* elapsed time */
static	double	percent;	/* 100 / etime */

/*
 * List of functions to be called which will construct the desired output
 */
static format_t	*formatter_list;
static format_t *formatter_end;

static uint64_t	hrtime_delta(hrtime_t, hrtime_t);
static u_longlong_t	ull_delta(u_longlong_t, u_longlong_t);
static uint_t 	u32_delta(uint_t, uint_t);
static void setup(void (*nfunc)(void));
static void print_timestamp(void);
static void print_tty_hdr1(void);
static void print_tty_hdr2(void);
static void print_cpu_hdr1(void);
static void print_cpu_hdr2(void);
static void print_tty_data(void);
static void print_cpu_data(void);
static void print_err_hdr(void);
static void print_disk_header(void);
static void hdrout(void);
static void disk_errors(void);
static void do_newline(void);
static void push_out(const char *, ...);
static void printhdr(int);
static void printxhdr(void);
static void usage(void);
static void do_args(int, char **);
static void do_format(void);
static void set_timer(int);
static void handle_sig(int);
static void show_all_disks(void);
static void show_first_disk(void);
static void show_other_disks(void);
static void show_disk_errors(void *, void *, void *);
static void write_core_header(void);
static int  fzero(double value);
static int  safe_strtoi(char const *val, char *errmsg);

int
main(int argc, char **argv)
{
	enum snapshot_types types = SNAP_SYSTEM;
	kstat_ctl_t *kc;
	long hz;

	do_args(argc, argv);
	do_format();

	/*
	 * iostat historically showed CPU changes, even though
	 * it doesn't provide much useful information
	 */
	types |= SNAP_CPUS;

	if (do_disk)
		types |= SNAP_IODEVS;

	if (do_disk && !do_partitions_only)
		df.if_allowed_types |= IODEV_DISK;
	if (do_disk & DISK_IOPATH) {
		df.if_allowed_types |= IODEV_IOPATH;
		types |= SNAP_IOPATHS;
	}
	if (do_disk & DISK_ERROR_MASK)
		types |= SNAP_IODEV_ERRORS;
	if (do_partitions || do_partitions_only)
		df.if_allowed_types |= IODEV_PARTITION;
	if (do_conversions)
		types |= SNAP_IODEV_PRETTY;
	if (do_controller) {
		if (!(do_disk & PRINT_VERTICAL) ||
			(do_disk & DISK_EXTENDED_ERRORS))
			fail(0, "-C can only be used with -e or -x.");
		types |= SNAP_CONTROLLERS;
		df.if_allowed_types |= IODEV_CONTROLLER;
	}

	hz = sysconf(_SC_CLK_TCK);

	/*
	 * Undocumented behavior - sending a SIGCONT will result
	 * in a new header being emitted. Used only if we're not
	 * doing extended headers. This is a historical
	 * artifact.
	 */
	if (!(do_disk & PRINT_VERTICAL))
		(void) signal(SIGCONT, printhdr);

	if (interval)
		set_timer(interval);

	kc = open_kstat();
	newss = acquire_snapshot(kc, types, &df);

	do {
		if (do_tty || do_cpu) {
			kstat_t *oldks;
			oldks = oldss ? &oldss->s_sys.ss_agg_sys : NULL;
			getime = cpu_ticks_delta(oldks,
			    &newss->s_sys.ss_agg_sys);
			percent = (getime > 0.0) ? 100.0 / getime : 0.0;
			getime = (getime / nr_active_cpus(newss)) / hz;
			if (getime == 0.0)
				getime = (double)interval;
			if (getime == 0.0 || do_interval)
				getime = 1.0;
		}

		if (formatter_list) {
			format_t *tmp;
			tmp = formatter_list;
			while (tmp) {
				(tmp->nfunc)();
				tmp = tmp->next;
			}
			(void) fflush(stdout);
		}

		if (interval > 0 && iter != 1)
			(void) pause();

		free_snapshot(oldss);
		oldss = newss;
		newss = acquire_snapshot(kc, types, &df);

		if (!suppress_state)
			snapshot_report_changes(oldss, newss);

		/* if config changed, show stats from boot */
		if (snapshot_has_changed(oldss, newss)) {
			free_snapshot(oldss);
			oldss = NULL;
		}

	} while (--iter);

	free_snapshot(oldss);
	free_snapshot(newss);
	return (0);
}

/*
 * Some magic numbers used in header formatting.
 *
 * DISK_LEN = length of either "kps tps serv" or "wps rps util"
 *	      using 0 as the first position
 *
 * DISK_ERROR_LEN = length of "s/w h/w trn tot" with one space on
 *		either side. Does not use zero as first pos.
 *
 * DEVICE_LEN = length of "device" + 1 character.
 */

#define	DISK_LEN	11
#define	DISK_ERROR_LEN	16
#define	DEVICE_LEN	7

/*ARGSUSED*/
static void
show_disk_name(void *v1, void *v2, void *data)
{
	struct iodev_snapshot *dev = (struct iodev_snapshot *)v2;
	size_t slen;
	char *name;
	char fbuf[SMALL_SCRATCH_BUFLEN];

	if (dev == NULL)
		return;

	name = dev->is_pretty ? dev->is_pretty : dev->is_name;
	if (!do_raw) {
		uint_t width;

		slen = strlen(name);
		/*
		 * The length is less
		 * than the section
		 * which will be displayed
		 * on the next line.
		 * Center the entry.
		 */

		width = (DISK_LEN + 1)/2 + (slen / 2);
		(void) snprintf(fbuf, sizeof (fbuf),
		    "%*s", width, name);
		name = fbuf;
		push_out("%-14.14s", name);
	} else {
		push_out(name);
	}
}

/*ARGSUSED*/
static void
show_disk_header(void *v1, void *v2, void *data)
{
	push_out(disk_header);
}

/*
 * Write out a two line header. What is written out depends on the flags
 * selected but in the worst case consists of a tty header, a disk header
 * providing information for 4 disks and a cpu header.
 *
 * The tty header consists of the word "tty" on the first line above the
 * words "tin tout" on the next line. If present the tty portion consumes
 * the first 10 characters of each line since "tin tout" is surrounded
 * by single spaces.
 *
 * Each of the disk sections is a 14 character "block" in which the name of
 * the disk is centered in the first 12 characters of the first line.
 *
 * The cpu section is an 11 character block with "cpu" centered over the
 * section.
 *
 * The worst case should look as follows:
 *
 * 0---------1--------2---------3---------4---------5---------6---------7-------
 *    tty        sd0           sd1           sd2           sd3           cpu
 *  tin tout kps tps serv  kps tps serv  kps tps serv  kps tps serv  us sy wt id
 *  NNN NNNN NNN NNN NNNN  NNN NNN NNNN  NNN NNN NNNN  NNN NNN NNNN  NN NN NN NN
 *
 * When -D is specified, the disk header looks as follows (worst case):
 *
 * 0---------1--------2---------3---------4---------5---------6---------7-------
 *     tty        sd0           sd1             sd2          sd3          cpu
 *   tin tout rps wps util  rps wps util  rps wps util  rps wps util us sy wt id
 *   NNN NNNN NNN NNN NNNN  NNN NNN NNNN  NNN NNN NNNN  NNN NNN NNNN NN NN NN NN
 */
static void
printhdr(int sig)
{
	/*
	 * If we're here because a signal fired, reenable the
	 * signal.
	 */
	if (sig)
		(void) signal(SIGCONT, printhdr);
	/*
	 * Horizontal mode headers
	 *
	 * First line
	 */
	if (do_tty)
		print_tty_hdr1();

	if (do_disk & DISK_NORMAL) {
		(void) snapshot_walk(SNAP_IODEVS, NULL, newss,
		    show_disk_name, NULL);
	}

	if (do_cpu)
		print_cpu_hdr1();
	do_newline();

	/*
	 * Second line
	 */
	if (do_tty)
		print_tty_hdr2();

	if (do_disk & DISK_NORMAL) {
		(void) snapshot_walk(SNAP_IODEVS, NULL, newss,
		    show_disk_header, NULL);
	}

	if (do_cpu)
		print_cpu_hdr2();
	do_newline();

	tohdr = REPRINT;
}

/*
 * Write out the extended header centered over the core information.
 */
static void
write_core_header(void)
{
	char *edev = "extended device statistics";
	uint_t lead_space_ct;
	uint_t follow_space_ct;
	size_t edevlen;

	if (do_raw == 0) {
		/*
		 * The things we do to look nice...
		 *
		 * Center the core output header. Make sure we have the
		 * right number of trailing spaces for follow-on headers
		 * (i.e., cpu and/or tty and/or errors).
		 */
		edevlen = strlen(edev);
		lead_space_ct = dh_len - edevlen;
		lead_space_ct /= 2;
		if (lead_space_ct > 0) {
			follow_space_ct = dh_len - (lead_space_ct + edevlen);
			if (do_disk & DISK_ERRORS)
				follow_space_ct -= DISK_ERROR_LEN;
			if ((do_disk & DISK_EXTENDED) && do_conversions)
				follow_space_ct -= DEVICE_LEN;

			push_out("%1$*2$.*2$s%3$s%4$*5$.*5$s", one_blank,
			    lead_space_ct, edev, one_blank, follow_space_ct);
		} else
			push_out("%56s", edev);
	} else
		push_out(edev);
}

/*
 * In extended mode headers, we don't want to reprint the header on
 * signals as they are printed every time anyways.
 */
static void
printxhdr(void)
{

	/*
	 * Vertical mode headers
	 */
	if (do_disk & DISK_EXTENDED)
		setup(write_core_header);
	if (do_disk & DISK_ERRORS)
		setup(print_err_hdr);

	if (do_conversions) {
		setup(do_newline);
		if (do_disk & (DISK_EXTENDED | DISK_ERRORS))
			setup(print_disk_header);
		setup(do_newline);
	} else {
		if (do_tty)
			setup(print_tty_hdr1);
		if (do_cpu)
			setup(print_cpu_hdr1);
		setup(do_newline);

		if (do_disk & (DISK_EXTENDED | DISK_ERRORS))
			setup(print_disk_header);
		if (do_tty)
			setup(print_tty_hdr2);
		if (do_cpu)
			setup(print_cpu_hdr2);
		setup(do_newline);
	}
}

/*
 * Write out a line for this disk - note that show_disk writes out
 * full lines or blocks for each selected disk.
 */
static void
show_disk(void *v1, void *v2, void *data)
{
	struct iodev_snapshot *old = (struct iodev_snapshot *)v1;
	struct iodev_snapshot *new = (struct iodev_snapshot *)v2;
	int *count = (int *)data;
	double rps, wps, tps, mtps, krps, kwps, kps, avw, avr, w_pct, r_pct;
	double wserv, rserv, serv;
	double iosize;	/* kb/sec or MB/sec */
	double etime, hr_etime;
	char *disk_name;
	u_longlong_t ldeltas;
	uint_t udeltas;
	uint64_t t_delta;
	uint64_t w_delta;
	uint64_t r_delta;
	int doit = 1;
	int i;
	uint_t toterrs;
	char *fstr;

	if (new == NULL)
		return;

	switch (show_disk_mode) {
	case SHOW_FIRST_ONLY:
		if (count != NULL && *count)
			return;
		break;

	case SHOW_SECOND_ONWARDS:
		if (count != NULL && !*count) {
			(*count)++;
			return;
		}
		break;

	default:
		break;
	}

	disk_name = new->is_pretty ? new->is_pretty : new->is_name;

	/*
	 * Only do if we want IO stats - Avoids errors traveling this
	 * section if that's all we want to see.
	 */
	if (do_disk & DISK_IO_MASK) {
		if (old) {
			t_delta = hrtime_delta(old->is_snaptime,
			    new->is_snaptime);
		} else {
			t_delta = hrtime_delta(new->is_crtime,
			    new->is_snaptime);
		}

		if (new->is_type == IODEV_CONTROLLER && new->is_nr_children)
			t_delta /= new->is_nr_children;

		hr_etime = (double)t_delta;
		if (hr_etime == 0.0)
			hr_etime = (double)NANOSEC;
		etime = hr_etime / (double)NANOSEC;

		/* reads per second */
		udeltas = u32_delta(old ? old->is_stats.reads : 0,
		    new->is_stats.reads);
		rps = (double)udeltas;
		rps /= etime;

		/* writes per second */
		udeltas = u32_delta(old ? old->is_stats.writes : 0,
		    new->is_stats.writes);
		wps = (double)udeltas;
		wps /= etime;

		tps = rps + wps;
			/* transactions per second */

		/*
		 * report throughput as either kb/sec or MB/sec
		 */

		if (!do_megabytes)
			iosize = 1024.0;
		else
			iosize = 1048576.0;

		ldeltas = ull_delta(old ? old->is_stats.nread : 0,
		    new->is_stats.nread);
		if (ldeltas) {
			krps = (double)ldeltas;
			krps /= etime;
			krps /= iosize;
		} else
			krps = 0.0;

		ldeltas = ull_delta(old ? old->is_stats.nwritten : 0,
		    new->is_stats.nwritten);
		if (ldeltas) {
			kwps = (double)ldeltas;
			kwps /= etime;
			kwps /= iosize;
		} else
			kwps = 0.0;

		/*
		 * Blocks transferred per second
		 */
		kps = krps + kwps;

		/*
		 * Average number of wait transactions waiting
		 */
		w_delta = hrtime_delta((u_longlong_t)
		    (old ? old->is_stats.wlentime : 0),
		    new->is_stats.wlentime);
		if (w_delta) {
			avw = (double)w_delta;
			avw /= hr_etime;
		} else
			avw = 0.0;

		/*
		 * Average number of run transactions waiting
		 */
		r_delta = hrtime_delta(old ? old->is_stats.rlentime : 0,
		    new->is_stats.rlentime);
		if (r_delta) {
			avr = (double)r_delta;
			avr /= hr_etime;
		} else
			avr = 0.0;

		/*
		 * Average wait service time in milliseconds
		 */
		if (tps > 0.0 && (avw != 0.0 || avr != 0.0)) {
			mtps = 1000.0 / tps;
			if (avw != 0.0)
				wserv = avw * mtps;
			else
				wserv = 0.0;

			if (avr != 0.0)
				rserv = avr * mtps;
			else
				rserv = 0.0;
			serv = rserv + wserv;
		} else {
			rserv = 0.0;
			wserv = 0.0;
			serv = 0.0;
		}

		/* % of time there is a transaction waiting for service */
		t_delta = hrtime_delta(old ? old->is_stats.wtime : 0,
		    new->is_stats.wtime);
		if (t_delta) {
			w_pct = (double)t_delta;
			w_pct /= hr_etime;
			w_pct *= 100.0;

			/*
			 * Average the wait queue utilization over the
			 * the controller's devices, if this is a controller.
			 */
			if (new->is_type == IODEV_CONTROLLER)
				w_pct /= new->is_nr_children;
		} else
			w_pct = 0.0;

		/* % of time there is a transaction running */
		t_delta = hrtime_delta(old ? old->is_stats.rtime : 0,
		    new->is_stats.rtime);
		if (t_delta) {
			r_pct = (double)t_delta;
			r_pct /= hr_etime;
			r_pct *= 100.0;

			/*
			 * Average the percent busy over the controller's
			 * devices, if this is a controller.
			 */
			if (new->is_type == IODEV_CONTROLLER)
				w_pct /= new->is_nr_children;
		} else {
			r_pct = 0.0;
		}

		/* % of time there is a transaction running */
		if (do_interval) {
			rps	*= etime;
			wps	*= etime;
			tps	*= etime;
			krps	*= etime;
			kwps	*= etime;
			kps	*= etime;
		}
	}

	if (do_disk & (DISK_EXTENDED | DISK_ERRORS)) {
		if ((!do_conversions) && ((suppress_zero == 0) ||
		    ((do_disk & DISK_EXTENDED) == 0))) {
			if (do_raw == 0)
				push_out(DISPLAYED_NAME_FORMAT,
				    disk_name);
			else
				push_out(disk_name);
		}
	}

	switch (do_disk & DISK_IO_MASK) {
	    case DISK_OLD:
		if (do_raw == 0)
			fstr = "%3.0f %3.0f %4.0f  ";
		else
			fstr = "%.0f,%.0f,%.0f";
		push_out(fstr, kps, tps, serv);
		break;
	    case DISK_NEW:
		if (do_raw == 0)
			fstr = "%3.0f %3.0f %4.1f  ";
		else
			fstr = "%.0f,%.0f,%.1f";
		push_out(fstr, rps, wps, r_pct);
		break;
	    case DISK_EXTENDED:
		if (suppress_zero) {
			if (fzero(rps) && fzero(wps) && fzero(krps) &&
			    fzero(kwps) && fzero(avw) && fzero(avr) &&
			    fzero(serv) && fzero(w_pct) && fzero(r_pct))
				doit = 0;
			else if (do_conversions == 0) {
				if (do_raw == 0)
					push_out(DISPLAYED_NAME_FORMAT,
					    disk_name);
				else
					push_out(disk_name);
			}
		}
		if (doit) {
			if (!do_conversions) {
				if (do_raw == 0) {
					fstr = " %6.1f %6.1f %6.1f %6.1f "
						"%4.1f %4.1f %6.1f %3.0f "
						"%3.0f ";
				} else {
					fstr = "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
						"%.1f,%.0f,%.0f";
				}
				push_out(fstr, rps, wps, krps, kwps, avw, avr,
				    serv, w_pct, r_pct);
			} else {
				if (do_raw == 0) {
					fstr = " %6.1f %6.1f %6.1f %6.1f "
						"%4.1f %4.1f %6.1f %6.1f "
						"%3.0f %3.0f ";
				} else {
					fstr = "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
						"%.1f,%.1f,%.0f,%.0f";
				}
				push_out(fstr, rps, wps, krps, kwps, avw, avr,
				    wserv, rserv, w_pct, r_pct);
			}
		}
		break;
	}

	if (do_disk & DISK_ERRORS) {
		if ((do_disk == DISK_ERRORS)) {
			if (do_raw == 0)
				push_out(two_blanks);
		}

		if (new->is_errors.ks_data) {
			kstat_named_t *knp;
			char *efstr;

			if (do_raw == 0)
				efstr = "%3u ";
			else
				efstr = "%u";
			toterrs = 0;
			knp = KSTAT_NAMED_PTR(&new->is_errors);
			for (i = 0; i < 3; i++) {
				switch (knp[i].data_type) {
					case KSTAT_DATA_ULONG:
						push_out(efstr,
						    knp[i].value.ui32);
						toterrs += knp[i].value.ui32;
						break;
					case KSTAT_DATA_ULONGLONG:
						/*
						 * We're only set up to
						 * write out the low
						 * order 32-bits so
						 * just grab that.
						 */
						push_out(efstr,
						    knp[i].value.ui32);
						toterrs += knp[i].value.ui32;
						break;
					default:
						break;
				}
			}
			push_out(efstr, toterrs);
		} else {
			if (do_raw == 0)
				push_out("  0   0   0   0 ");
			else
				push_out("0,0,0,0");
		}

	}

	if (suppress_zero == 0 || doit == 1) {
		if ((do_disk & (DISK_EXTENDED | DISK_ERRORS)) &&
			do_conversions) {
			push_out("%s", disk_name);
			if (show_mountpts && new->is_dname) {
				mnt_t *mount_pt;
				char *lu;
				char lub[SMALL_SCRATCH_BUFLEN];

				lu = strrchr(new->is_dname, '/');
				if (lu) {
					if (strcmp(disk_name, lu) == 0)
						lu = new->is_dname;
					else {
						*lu = 0;
						(void) strcpy(lub,
						    new->is_dname);
						*lu = '/';
						(void) strcat(lub, "/");
						(void) strcat(lub,
						    disk_name);
						lu = lub;
					}
				} else
					lu = disk_name;
				mount_pt = lookup_mntent_byname(lu);
				if (mount_pt) {
					if (do_raw == 0)
						push_out(" (%s)",
						    mount_pt->mount_point);
					else
						push_out("(%s)",
						    mount_pt->mount_point);
				}
			}
		}
	}

	if ((do_disk & PRINT_VERTICAL) && show_disk_mode != SHOW_FIRST_ONLY)
		do_newline();

	if (count != NULL)
		(*count)++;
}

static void
usage(void)
{
	(void) fprintf(stderr,
	    "Usage: iostat [-cCdDeEiImMnpPrstxXz] "
	    " [-l n] [-T d|u] [disk ...] [interval [count]]\n"
	    "\t\t-c: 	report percentage of time system has spent\n"
	    "\t\t\tin user/system/wait/idle mode\n"
	    "\t\t-C: 	report disk statistics by controller\n"
	    "\t\t-d: 	display disk Kb/sec, transfers/sec, avg. \n"
	    "\t\t\tservice time in milliseconds  \n"
	    "\t\t-D: 	display disk reads/sec, writes/sec, \n"
	    "\t\t\tpercentage disk utilization \n"
	    "\t\t-e: 	report device error summary statistics\n"
	    "\t\t-E: 	report extended device error statistics\n"
	    "\t\t-i:	show device IDs for -E output\n"
	    "\t\t-I: 	report the counts in each interval,\n"
	    "\t\t\tinstead of rates, where applicable\n"
	    "\t\t-l n:	Limit the number of disks to n\n"
	    "\t\t-m: 	Display mount points (most useful with -p)\n"
	    "\t\t-M: 	Display data throughput in MB/sec "
	    "instead of Kb/sec\n"
	    "\t\t-n: 	convert device names to cXdYtZ format\n"
	    "\t\t-p: 	report per-partition disk statistics\n"
	    "\t\t-P: 	report per-partition disk statistics only,\n"
	    "\t\t\tno per-device disk statistics\n"
	    "\t\t-r: 	Display data in comma separated format\n"
	    "\t\t-s: 	Suppress state change messages\n"
	    "\t\t-T d|u	Display a timestamp in date (d) or unix "
	    "time_t (u)\n"
	    "\t\t-t: 	display chars read/written to terminals\n"
	    "\t\t-x: 	display extended disk statistics\n"
	    "\t\t-X: 	display I/O path statistics\n"
	    "\t\t-z: 	Suppress entries with all zero values\n");
	exit(1);
}

/*ARGSUSED*/
static void
show_disk_errors(void *v1, void *v2, void *d)
{
	struct iodev_snapshot *disk = (struct iodev_snapshot *)v2;
	kstat_named_t *knp;
	size_t  col;
	int	i, len;
	char	*dev_name = disk->is_name;

	if (disk->is_errors.ks_ndata == 0)
		return;
	if (disk->is_type == IODEV_CONTROLLER)
		return;

	if (disk->is_pretty)
		dev_name = disk->is_pretty;

	len = strlen(dev_name);
	if (len > 20)
		push_out("%s ", dev_name);
	else if (len > 16)
		push_out("%-20.20s ", dev_name);
	else {
		if (do_conversions)
			push_out("%-16.16s ", dev_name);
		else
			push_out("%-9.9s ", dev_name);
	}
	col = 0;

	knp = KSTAT_NAMED_PTR(&disk->is_errors);
	for (i = 0; i < disk->is_errors.ks_ndata; i++) {
		/* skip kstats that the driver did not kstat_named_init */
		if (knp[i].name[0] == 0)
			continue;

		col += strlen(knp[i].name);

		switch (knp[i].data_type) {
			case KSTAT_DATA_CHAR:
				if ((strcmp(knp[i].name, "Serial No") == 0) &&
				    do_devid) {
					if (disk->is_devid) {
						push_out("Device Id: %s ",
						    disk->is_devid);
						col += strlen(disk->is_devid);
					} else
						push_out("Device Id: ");
				} else {
					push_out("%s: %-.16s ", knp[i].name,
					    &knp[i].value.c[0]);
					col += strlen(&knp[i].value.c[0]);
				}
				break;
			case KSTAT_DATA_ULONG:
				push_out("%s: %u ", knp[i].name,
				    knp[i].value.ui32);
				col += 4;
				break;
			case KSTAT_DATA_ULONGLONG:
				if (strcmp(knp[i].name, "Size") == 0) {
					push_out("%s: %2.2fGB <%llu bytes>\n",
					    knp[i].name,
					    (float)knp[i].value.ui64 /
					    DISK_GIGABYTE,
					    knp[i].value.ui64);
					col = 0;
					break;
				}
				push_out("%s: %u ", knp[i].name,
				    knp[i].value.ui32);
				col += 4;
				break;
			}
		if ((col >= 62) || (i == 2)) {
			do_newline();
			col = 0;
		}
	}
	if (col > 0) {
		do_newline();
	}
	do_newline();
}

void
do_args(int argc, char **argv)
{
	int 		c;
	int 		errflg = 0;
	extern char 	*optarg;
	extern int 	optind;

	while ((c = getopt(argc, argv, "tdDxXCciIpPnmMeEszrT:l:")) != EOF)
		switch (c) {
		case 't':
			do_tty++;
			break;
		case 'd':
			do_disk |= DISK_OLD;
			break;
		case 'D':
			do_disk |= DISK_NEW;
			break;
		case 'x':
			do_disk |= DISK_EXTENDED;
			break;
		case 'X':
			do_disk |= DISK_IOPATH;
			break;
		case 'C':
			do_controller++;
			break;
		case 'c':
			do_cpu++;
			break;
		case 'I':
			do_interval++;
			break;
		case 'p':
			do_partitions++;
			break;
		case 'P':
			do_partitions_only++;
			break;
		case 'n':
			do_conversions++;
			break;
		case 'M':
			do_megabytes++;
			break;
		case 'e':
			do_disk |= DISK_ERRORS;
			break;
		case 'E':
			do_disk |= DISK_EXTENDED_ERRORS;
			break;
		case 'i':
			do_devid = 1;
			break;
		case 's':
			suppress_state = 1;
			break;
		case 'z':
			suppress_zero = 1;
			break;
		case 'm':
			show_mountpts = 1;
			break;
		case 'T':
			if (optarg) {
				if (*optarg == 'u')
					do_timestamp = UDATE;
				else if (*optarg == 'd')
					do_timestamp = CDATE;
				else
					errflg++;
			} else
				errflg++;
			break;
		case 'r':
			do_raw = 1;
			break;
		case 'l':
			df.if_max_iodevs = safe_strtoi(optarg, "invalid limit");
			if (df.if_max_iodevs < 1)
				usage();
			break;
		case '?':
			errflg++;
	}

	if ((do_disk & DISK_OLD) && (do_disk & DISK_NEW)) {
		(void) fprintf(stderr, "-d and -D are incompatible.\n");
		usage();
	}

	if (errflg) {
		usage();
	}
	/* if no output classes explicity specified, use defaults */
	if (do_tty == 0 && do_disk == 0 && do_cpu == 0)
		do_tty = do_cpu = 1, do_disk = DISK_OLD;

	/*
	 * If conflicting options take the preferred
	 * -D and -x result in -x
	 * -d or -D and -e or -E gives only whatever -d or -D was specified
	 */
	if ((do_disk & DISK_EXTENDED) && (do_disk & DISK_NORMAL))
		do_disk &= ~DISK_NORMAL;
	if ((do_disk & DISK_NORMAL) && (do_disk & DISK_ERROR_MASK))
		do_disk &= ~DISK_ERROR_MASK;

	/*
	 * I/O path stats are only available with extended (-x) stats
	 */
	if ((do_disk & DISK_IOPATH) && !(do_disk & DISK_EXTENDED))
		do_disk &= ~DISK_IOPATH;

	/* nfs, tape, always shown */
	df.if_allowed_types = IODEV_NFS | IODEV_TAPE;

	/*
	 * If limit == 0 then no command line limit was set, else if any of
	 * the flags that cause unlimited disks were not set,
	 * use the default of 4
	 */
	if (df.if_max_iodevs == 0) {
		df.if_max_iodevs = DEFAULT_LIMIT;
		df.if_skip_floppy = 1;
		if (do_disk & (DISK_EXTENDED | DISK_ERRORS |
		    DISK_EXTENDED_ERRORS)) {
			df.if_max_iodevs = UNLIMITED_IODEVS;
			df.if_skip_floppy = 0;
		}
	}
	if (do_disk) {
		size_t count = 0;
		size_t i = optind;

		while (i < argc && !isdigit(argv[i][0])) {
			count++;
			i++;
		}

		/*
		 * "Note:  disks  explicitly  requested
		 * are not subject to this disk limit"
		 */
		if (count > df.if_max_iodevs)
			df.if_max_iodevs = count;
		df.if_names = safe_alloc(count * sizeof (char *));
		(void) memset(df.if_names, 0, count * sizeof (char *));

		while (optind < argc && !isdigit(argv[optind][0]))
			df.if_names[df.if_nr_names++] = argv[optind++];
	}
	if (optind < argc) {
		interval = safe_strtoi(argv[optind], "invalid interval");
		if (interval < 1)
			fail(0, "invalid interval");
		optind++;

		if (optind < argc) {
			iter = safe_strtoi(argv[optind], "invalid count");
			if (iter < 1)
				fail(0, "invalid count");
			optind++;
		}
	}
	if (interval == 0)
		iter = 1;
	if (optind < argc)
		usage();
}

/*
 * Driver for doing the extended header formatting. Will produce
 * the function stack needed to output an extended header based
 * on the options selected.
 */

void
do_format(void)
{
	char	header[SMALL_SCRATCH_BUFLEN];
	char 	ch;
	char 	iosz;
	const char    *fstr;

	disk_header[0] = 0;
	ch = (do_interval ? 'i' : 's');
	iosz = (do_megabytes ? 'M' : 'k');
	if (do_disk & DISK_ERRORS) {
		if (do_raw == 0) {
			(void) sprintf(header, "s/w h/w trn tot ");
		} else
			(void) sprintf(header, "s/w,h/w,trn,tot");
	} else
		*header = NULL;
	switch (do_disk & DISK_IO_MASK) {
		case DISK_OLD:
			if (do_raw == 0)
				fstr = "%cp%c tp%c serv  ";
			else
				fstr = "%cp%c,tp%c,serv";
			(void) snprintf(disk_header, sizeof (disk_header),
			    fstr, iosz, ch, ch);
			break;
		case DISK_NEW:
			if (do_raw == 0)
				fstr = "rp%c wp%c util  ";
			else
				fstr = "%rp%c,wp%c,util";
			(void) snprintf(disk_header, sizeof (disk_header),
			    fstr, ch, ch);
			break;
		case DISK_EXTENDED:
			if (!do_conversions) {
				if (do_raw == 0)
					fstr = "device       r/%c    w/%c   "
					    "%cr/%c   %cw/%c wait actv  "
					    "svc_t  %%%%w  %%%%b %s";
				else
					fstr = "device,r/%c,w/%c,%cr/%c,%cw/%c,"
						"wait,actv,svc_t,%%%%w,"
						"%%%%b,%s";
				(void) snprintf(disk_header,
				    sizeof (disk_header),
				    fstr, ch, ch, iosz, ch, iosz,
				    ch, header);
			} else {
				if (do_raw == 0) {
					fstr = "    r/%c    w/%c   %cr/%c   "
					    "%cw/%c wait actv wsvc_t asvc_t  "
					    "%%%%w  %%%%b %sdevice";
				} else {
					fstr = "r/%c,w/%c,%cr/%c,%cw/%c,"
					    "wait,actv,wsvc_t,asvc_t,"
					    "%%%%w,%%%%b,%sdevice";
				}
				(void) snprintf(disk_header,
				    sizeof (disk_header),
				    fstr, ch, ch, iosz, ch, iosz,
				    ch, header);
			}
			break;
		default:
			break;
	}
	if (do_disk == DISK_ERRORS) {
		char *sep;

		if (!do_conversions) {
			if (do_raw == 0) {
				sep = "     ";
			} else
				sep = ",";
			(void) snprintf(disk_header, sizeof (disk_header),
			    "%s%s%s", "device", sep, header);
		} else {
			if (do_raw == 0) {
				(void) snprintf(disk_header,
				    sizeof (disk_header),
				    "  %s", header);
			} else
				(void) strcpy(disk_header, header);
		}
	} else {
		/*
		 * Need to subtract two characters for the % escape in
		 * the string.
		 */
		dh_len = strlen(disk_header) - 2;
	}

	if (do_timestamp)
		setup(print_timestamp);

	/*
	 * -n *and* (-E *or* -e *or* -x)
	 */
	if (do_conversions && (do_disk & PRINT_VERTICAL)) {
		if (do_tty)
			setup(print_tty_hdr1);
		if (do_cpu)
			setup(print_cpu_hdr1);
		if (do_tty || do_cpu)
			setup(do_newline);
		if (do_tty)
			setup(print_tty_hdr2);
		if (do_cpu)
			setup(print_cpu_hdr2);
		if (do_tty || do_cpu)
			setup(do_newline);
		if (do_tty)
			setup(print_tty_data);
		if (do_cpu)
			setup(print_cpu_data);
		if (do_tty || do_cpu)
			setup(do_newline);
		printxhdr();

		setup(show_all_disks);
	} else {
		/*
		 * These unholy gymnastics are necessary to place CPU/tty
		 * data to the right of the disks/errors for the first
		 * line in vertical mode.
		 */
		if (do_disk & PRINT_VERTICAL) {
			printxhdr();

			setup(show_first_disk);
			if (do_tty)
				setup(print_tty_data);
			if (do_cpu)
				setup(print_cpu_data);
			setup(do_newline);

			setup(show_other_disks);
		} else {
			setup(hdrout);
			if (do_tty)
				setup(print_tty_data);
			setup(show_all_disks);
			if (do_cpu)
				setup(print_cpu_data);
		}

		setup(do_newline);
	}
	if (do_disk & DISK_EXTENDED_ERRORS)
		setup(disk_errors);
}

/*
 * Add a new function to the list of functions
 * for this invocation. Once on the stack the
 * function is never removed nor does its place
 * change.
 */
void
setup(void (*nfunc)(void))
{
	format_t *tmp;

	tmp = safe_alloc(sizeof (format_t));
	tmp->nfunc = nfunc;
	tmp->next = 0;
	if (formatter_end)
		formatter_end->next = tmp;
	else
		formatter_list = tmp;
	formatter_end = tmp;

}

/*
 * The functions after this comment are devoted to printing
 * various parts of the header. They are selected based on the
 * options provided when the program was invoked. The functions
 * are either directly invoked in printhdr() or are indirectly
 * invoked by being placed on the list of functions used when
 * extended headers are used.
 */
void
print_tty_hdr1(void)
{
	char *fstr;
	char *dstr;

	if (do_raw == 0) {
		fstr = "%10.10s";
		dstr = "tty    ";
	} else {
		fstr = "%s";
		dstr = "tty";
	}
	push_out(fstr, dstr);
}

void
print_tty_hdr2(void)
{
	if (do_raw == 0)
		push_out("%-10.10s", " tin tout");
	else
		push_out("tin,tout");
}

void
print_cpu_hdr1(void)
{
	char *dstr;

	if (do_raw == 0)
		dstr = "     cpu";
	else
		dstr = "cpu";
	push_out(dstr);
}

void
print_cpu_hdr2(void)
{
	char *dstr;

	if (do_raw == 0)
		dstr = " us sy wt id";
	else
		dstr = "us,sy,wt,id";
	push_out(dstr);
}

/*
 * Assumption is that tty data is always first - no need for raw mode leading
 * comma.
 */
void
print_tty_data(void)
{
	char *fstr;
	uint64_t deltas;
	double raw;
	double outch;
	kstat_t *oldks = NULL;

	if (oldss)
		oldks = &oldss->s_sys.ss_agg_sys;

	if (do_raw == 0)
		fstr = " %3.0f %4.0f ";
	else
		fstr = "%.0f,%.0f";
	deltas = kstat_delta(oldks, &newss->s_sys.ss_agg_sys, "rawch");
	raw = deltas;
	raw /= getime;
	deltas = kstat_delta(oldks, &newss->s_sys.ss_agg_sys, "outch");
	outch = deltas;
	outch /= getime;
	push_out(fstr, raw, outch);
}

/*
 * Write out CPU data
 */
void
print_cpu_data(void)
{
	char *fstr;
	uint64_t idle;
	uint64_t user;
	uint64_t kern;
	uint64_t wait;
	kstat_t *oldks = NULL;

	if (oldss)
		oldks = &oldss->s_sys.ss_agg_sys;

	if (do_raw == 0)
		fstr = " %2.0f %2.0f %2.0f %2.0f";
	else
		fstr = "%.0f,%.0f,%.0f,%.0f";

	idle = kstat_delta(oldks, &newss->s_sys.ss_agg_sys, "cpu_ticks_idle");
	user = kstat_delta(oldks, &newss->s_sys.ss_agg_sys, "cpu_ticks_user");
	kern = kstat_delta(oldks, &newss->s_sys.ss_agg_sys, "cpu_ticks_kernel");
	wait = kstat_delta(oldks, &newss->s_sys.ss_agg_sys, "cpu_ticks_wait");
	push_out(fstr, user * percent, kern * percent,
		wait * percent, idle * percent);
}

/*
 * Emit the appropriate header.
 */
void
hdrout(void)
{
	if (do_raw == 0) {
		if (--tohdr == 0)
			printhdr(0);
	} else if (hdr_out == 0) {
		printhdr(0);
		hdr_out = 1;
	}
}

/*
 * Write out disk errors when -E is specified.
 */
void
disk_errors(void)
{
	(void) snapshot_walk(SNAP_IODEVS, oldss, newss, show_disk_errors, NULL);
}

void
show_first_disk(void)
{
	int count = 0;

	show_disk_mode = SHOW_FIRST_ONLY;

	(void) snapshot_walk(SNAP_IODEVS, oldss, newss, show_disk, &count);
}

void
show_other_disks(void)
{
	int count = 0;

	show_disk_mode = SHOW_SECOND_ONWARDS;

	(void) snapshot_walk(SNAP_IODEVS, oldss, newss, show_disk, &count);
}

void
show_all_disks(void)
{
	int count = 0;

	show_disk_mode = SHOW_ALL;

	(void) snapshot_walk(SNAP_IODEVS, oldss, newss, show_disk, &count);
}

/*
 * Write a newline out and clear the lineout flag.
 */
static void
do_newline(void)
{
	if (lineout) {
		(void) putchar('\n');
		lineout = 0;
	}
}

/*
 * Generalized printf function that determines what extra
 * to print out if we're in raw mode. At this time we
 * don't care about errors.
 */
static void
push_out(const char *message, ...)
{
	va_list args;

	va_start(args, message);
	if (do_raw && lineout == 1)
		(void) putchar(',');
	(void) vprintf(message, args);
	va_end(args);
	lineout = 1;
}

/*
 * Emit the header string when -e is specified.
 */
static void
print_err_hdr(void)
{
	char obuf[SMALL_SCRATCH_BUFLEN];

	if (do_conversions == 0) {
		if (!(do_disk & DISK_EXTENDED)) {
			(void) snprintf(obuf, sizeof (obuf),
			    "%11s", one_blank);
			push_out(obuf);
		}
	} else if (do_disk == DISK_ERRORS)
		push_out(two_blanks);
	else
		push_out(one_blank);
	push_out("---- errors --- ");
}

/*
 * Emit the header string when -e is specified.
 */
static void
print_disk_header(void)
{
	push_out(disk_header);
}

/*
 * Write out a timestamp. Format is all that goes out on
 * the line so no use of push_out.
 *
 * Write out as decimal reprentation of time_t value
 * (-T u was specified) or the string returned from
 * ctime() (-T d was specified).
 */
static void
print_timestamp(void)
{
	time_t t;

	if (time(&t) != -1) {
		if (do_timestamp == UDATE) {
			(void) printf("%ld\n", t);
		} else if (do_timestamp == CDATE) {
			char *cpt;

			cpt = ctime(&t);
			if (cpt) {
				(void) fputs(cpt, stdout);
			}
		}
	}
}

/*
 * No, UINTMAX_MAX isn't the right thing here since
 * it is #defined to be either INT32_MAX or INT64_MAX
 * depending on the whether _LP64 is defined.
 *
 * We want to handle the odd future case of having
 * ulonglong_t be more than 64 bits but we have
 * no nice #define MAX value we can drop in place
 * without having to change this code in the future.
 */

u_longlong_t
ull_delta(u_longlong_t old, u_longlong_t new)
{
	if (new >= old)
		return (new - old);
	else
		return ((UINT64_MAX - old) + new + 1);
}

/*
 * Return the number of ticks delta between two hrtime_t
 * values. Attempt to cater for various kinds of overflow
 * in hrtime_t - no matter how improbable.
 */
uint64_t
hrtime_delta(hrtime_t old, hrtime_t new)
{
	uint64_t del;

	if ((new >= old) && (old >= 0L))
		return (new - old);
	else {
		/*
		 * We've overflowed the positive portion of an
		 * hrtime_t.
		 */
		if (new < 0L) {
			/*
			 * The new value is negative. Handle the
			 * case where the old value is positive or
			 * negative.
			 */
			uint64_t n1;
			uint64_t o1;

			n1 = -new;
			if (old > 0L)
				return (n1 - old);
			else {
				o1 = -old;
				del = n1 - o1;
				return (del);
			}
		} else {
			/*
			 * Either we've just gone from being negative
			 * to positive *or* the last entry was positive
			 * and the new entry is also positive but *less*
			 * than the old entry. This implies we waited
			 * quite a few days on a very fast system between
			 * iostat displays.
			 */
			if (old < 0L) {
				uint64_t o2;

				o2 = -old;
				del = UINT64_MAX - o2;
			} else {
				del = UINT64_MAX - old;
			}
			del += new;
			return (del);
		}
	}
}

/*
 * Take the difference of an unsigned 32
 * bit int attempting to cater for
 * overflow.
 */
uint_t
u32_delta(uint_t old, uint_t new)
{
	if (new >= old)
		return (new - old);
	else
		return ((UINT32_MAX - old) + new + 1);
}

/*
 * Create and arm the timer. Used only when an interval has been specified.
 * Used in lieu of poll to ensure that we provide info for exactly the
 * desired period.
 */
void
set_timer(int interval)
{
	timer_t t_id;
	itimerspec_t time_struct;
	struct sigevent sig_struct;
	struct sigaction act;

	bzero(&sig_struct, sizeof (struct sigevent));
	bzero(&act, sizeof (struct sigaction));

	/* Create timer */
	sig_struct.sigev_notify = SIGEV_SIGNAL;
	sig_struct.sigev_signo = SIGUSR1;
	sig_struct.sigev_value.sival_int = 0;

	if (timer_create(CLOCK_REALTIME, &sig_struct, &t_id) != 0) {
		fail(1, "Timer creation failed");
	}

	act.sa_handler = handle_sig;

	if (sigaction(SIGUSR1, &act, NULL) != 0) {
		fail(1, "Could not set up signal handler");
	}

	time_struct.it_value.tv_sec = interval;
	time_struct.it_value.tv_nsec = 0;
	time_struct.it_interval.tv_sec = interval;
	time_struct.it_interval.tv_nsec = 0;

	/* Arm timer */
	if ((timer_settime(t_id, 0, &time_struct, NULL)) != 0) {
		fail(1, "Setting timer failed");
	}
}
/* ARGSUSED */
void
handle_sig(int x)
{
}

/*
 * This is exactly what is needed for standard iostat output,
 * but make sure to use it only for that
 */
#define	EPSILON	(0.1)
static int
fzero(double value)
{
	return (value >= 0.0 && value < EPSILON);
}

static int
safe_strtoi(char const *val, char *errmsg)
{
	char *end;
	long tmp;

	errno = 0;
	tmp = strtol(val, &end, 10);
	if (*end != '\0' || errno)
		fail(0, "%s %s", errmsg, val);
	return ((int)tmp);
}