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
 *
 */

#pragma ident	"@(#)boot_plat.c	1.47	05/10/30 SMI"

#include <sys/param.h>
#include <sys/fcntl.h>
#include <sys/obpdefs.h>
#include <sys/reboot.h>
#include <sys/promif.h>
#include <sys/stat.h>
#include <sys/bootvfs.h>
#include <sys/platnames.h>
#include <sys/salib.h>
#include <sys/elf.h>
#include <sys/link.h>
#include <sys/auxv.h>
#include <sys/boot_policy.h>
#include <sys/boot_redirect.h>
#include <sys/bootconf.h>
#include <sys/boot.h>
#include "boot_plat.h"

#define	SUCCESS		0
#define	FAILURE		-1

#define	ISSPACE(c)		(c == ' ' || c == '\t')
#define	SKIP_WHITESPC(cp)	while (*cp && ISSPACE(*cp)) cp++;


#ifdef DEBUG
int debug = 1;
#else
static const int debug = 0;
#endif
#define dprintf         if (debug) printf

#ifdef DEBUG_LISTS
void print_memlist(struct memlist *av);
#endif

extern	int (*readfile(int fd, int print))();
extern	void kmem_init(void);
extern	void *kmem_alloc(size_t, int);
extern	void kmem_free(void *, size_t);
extern	void get_boot_args(char *buf);
extern	void setup_bootops(void);
extern	struct	bootops bootops;
extern	Elf32_Boot *elfbootvec;
extern	void exitto(int (*entrypoint)());
extern void exitto64(int (*entrypoint)(), void *bootvec);
extern void *p1275cif;

int openfile(char *filename);

/*
 * filename is the name of the standalone we're going to execute.
 */
char	filename[MAXPATHLEN];
char	*cmd_line_default_path;

/* char * const defname = "kernel/ppc/unix"; */
char * const defname = "ppc/unix"; /* for testing ufsboot */

/*
 *  We enable the cache by default
 *  but boot -n will leave it alone...
 *  that is, we use whatever state the PROM left it in.
 */
char	*mfg_name;
int	cache_state = 1;
char	filename2[MAXPATHLEN];

int	boothowto = RB_VERBOSE; /* verbose (for now) */
int	verbosemode = 1; /* same as above */

/*
 *  stubs for heap_kmem (assuming they're actually even needed there)
 */

static struct bootops *bopp;

int
splimp()
{
	        return (0);
}

/*ARGSUSED*/
void
splx(int rs)
{
}

int
splnet()
{
	        return (0);
}

void
exitto(int (*entrypoint)())
{
	bopp = &bootops;
	(*entrypoint)(0, 0, p1275cif, bopp, elfbootvec);
}

void
exitto64(int (*entrypoint)(void *romvec, void *dvec, void *bootops,
			    void *bootvec), void *bootvec)
{}

int
client_handler(void *cif_handler, void *arg_array)
{ 
	return ((int (*)(void *))cif_handler)(arg_array);
}


/*
 * Copy filename and bargs into v2args_buf, which will be exported as the
 * boot-args boot property.  We should probably warn the user if anything gets
 * cut off.
 */
void
set_client_bootargs(const char *filename, const char *bargs)
{
	int i = 0;
	const char *s;

	s = filename;
	while (*s != '\0' && i < V2ARGS_BUF_SZ - 1)
		v2args_buf[i++] = *s++;

	if (i >= V2ARGS_BUF_SZ - 2) {
		/* Not enough room for a space and any of bargs. */
		v2args_buf[i] = '\0';
		return;
	}

	v2args_buf[i++] = ' ';

	s = bargs;
	while (*s != '\0' && i < V2ARGS_BUF_SZ - 1)
		v2args_buf[i++] = *s++;

	v2args_buf[i] = '\0';
}

/*
 * The slice redirection file is used on the install CD
 */
static int
read_redirect(char *redirect)
{
	int fd;
	char slicec;
	size_t nread = 0;

	if ((fd = open(BOOT_REDIRECT, O_RDONLY)) != -1) {
		/*
		 * Read the character out of the file - this is the
		 * slice to use, in base 36.
		 */
		nread = read(fd, &slicec, 1);
		(void) close(fd);
		if (nread == 1)
			*redirect++ = slicec;
	}
	*redirect = '\0';

	return (nread == 1);
}

void
post_mountroot(char *bootfile, char *redirect)
{
	int (*go2)();
	int fd;

	/* Save the bootfile, just in case we need it again */
	(void) strcpy(filename2, bootfile);

	for (;;) {
		if (boothowto & RB_ASKNAME) {
			char tmpname[MAXPATHLEN];

			printf("Enter filename [%s]: ", bootfile);
			(void) cons_gets(tmpname, sizeof (tmpname));
			if (tmpname[0] != '\0')
				(void) strcpy(bootfile, tmpname);
		}

		if (boothowto & RB_HALT) {
			printf("Boot halted.\n");
			prom_enter_mon();
		}

		if ((fd = openfile(bootfile)) == FAILURE) {

			/*
			 * There are many reasons why this might've
			 * happened .. but one of them is that we're
			 * on the installation CD, and we need to
			 * revector ourselves off to a different partition
			 * of the CD.  Check for the redirection file.
			 */
			if (redirect != NULL &&
			    read_redirect(redirect)) {
				/* restore bootfile */
				(void) strcpy(bootfile, filename2);
				return;
				/*NOTREACHED*/
			}

			printf("%s: cannot open %s\n", my_own_name, bootfile);
			boothowto |= RB_ASKNAME;

			/* restore bootfile */
			(void) strcpy(bootfile, filename2);
			continue;
		}
		printf("Boot file (%s) opened!\n", bootfile);

		if ((go2 = readfile(fd, boothowto & RB_VERBOSE)) !=
		    (int(*)()) -1) {
#ifdef MPSAS
			sas_bpts();
#endif
			(void) close(fd);
		} else {
			printf("boot failed\n");
			boothowto |= RB_ASKNAME;
			continue;
		}

		if (boothowto & RB_HALT) {
			printf("Boot halted before exit to 0x%p.\n",
			    (void *)go2);
			prom_enter_mon();
		}

		my_own_name = bootfile;

		printf("Calling exitto(%p)\n", (void *)go2);
		exitto(go2);
	}
}

/*ARGSUSED*/
static int
boot_open(char *pathname, void *arg)
{
	dprintf("trying '%s'\n", pathname);
	return (open(pathname, O_RDONLY));
}

static int
boot_isdir(char *pathname)
{
	int fd, retval;
	struct stat sbuf;

	dprintf("trying '%s'\n", pathname);
	if ((fd = open(pathname, O_RDONLY)) == -1)
		return (0);
	retval = 1;
	if (fstat(fd, &sbuf) == -1)
		retval = 0;
	else if ((sbuf.st_mode & S_IFMT) != S_IFDIR)
		retval = 0;
	(void) close(fd);
	return (retval);
}

/*
 * Open the given filename, expanding to it's
 * platform-dependent location if necessary.
 *
 * Boot supports OBP and IEEE1275.
 *
 * XXX: Move side effects out of this function!
 */
int
openfile(char *filename)
{
	static char *fullpath;
	static char *iarch;
	static char *orig_impl_arch_name;
	static int once;
	int fd;

	if (once == 0) {

		++once;

		/*
		 * Setup exported 'boot' properties: 'mfg-name'.
		 * XXX: This shouldn't be a side effect of openfile().
		 */
		if (mfg_name == NULL)
			mfg_name = get_mfg_name();

		/*
		 * If impl_arch_name was specified on the command line
		 * via the -I <arch> argument, remember the original value.
		 */
		if (impl_arch_name) {
			orig_impl_arch_name = (char *)
			    kmem_alloc(strlen(impl_arch_name) + 1, 0);
			(void) strcpy(orig_impl_arch_name, impl_arch_name);
		}

		fullpath = (char *)kmem_alloc(MAXPATHLEN, 0);
		iarch = (char *)kmem_alloc(MAXPATHLEN, 0);
	}

	/*
	 * impl_arch_name is exported as boot property, and is
	 * set according to the following algorithm, depending
	 * on the contents of the filesystem.
	 * XXX: This shouldn't be a side effect of openfile().
	 *
	 * impl_arch_name table:
	 *
	 *			root name 	default name	neither name
	 * boot args		found		found		found
	 *
	 * relative path	root name	fail		fail
	 * absolute path	root name	default name	empty
	 * -I arch		arch		arch		arch
	 *
	 */

	/*
	 * If the caller -specifies- an absolute pathname, then we just try to
	 * open it. (Mostly for booting non-kernel standalones.)
	 *
	 * In case this absolute pathname is the kernel, make sure that
	 * impl_arch_name (exported as a boot property) is set to some
	 * valid string value.
	 */
	if (*filename == '/') {
		if (orig_impl_arch_name == NULL) {
			if (find_platform_dir(boot_isdir, iarch, 1) != 0)
				impl_arch_name = iarch;
			else
				impl_arch_name = "";
		}
		(void) strcpy(fullpath, filename);
		fd = boot_open(fullpath, NULL);
		return (fd);
	}

	/*
	 * If the -I flag has been used, impl_arch_name will
	 * be specified .. otherwise we ask find_platform_dir() to
	 * look for the existance of a directory for this platform name.
	 * Preserve the given impl-arch-name, because the 'kernel file'
	 * may be elsewhere. 
	 *
	 * When booting any file by relative pathname this code fails
	 * if the platform-name dir doesn't exist unless some
	 * -I <iarch> argument has been given on the command line.
	 */
	if (orig_impl_arch_name == NULL) {
		if (find_platform_dir(boot_isdir, iarch, 0) != 0)
			impl_arch_name = iarch;
		else
			return (-1);
	}

	fd = open_platform_file(filename, boot_open, NULL, fullpath,
	    orig_impl_arch_name);
	if (fd == -1)
		return (-1);

	/*
	 * Copy back the name we actually found
	 */
	(void) strcpy(filename, fullpath);
	return (fd);
}

/*
 * Get the boot arguments from the PROM and split it into filename and
 * options components.
 *
 * As per IEEE1275 and boot(1M), the boot arguments will have the syntax
 * "[filename] [-options]".  If filename is specified, it is copied into the
 * first buffer.  (Otherwise, the buffer is left alone.)  The rest of the string
 * is copied into the second buffer.
 */
static void
init_bootargs(char *fname_buf, int fname_buf_sz, char *bargs_buf,
    int bargs_buf_sz)
{
	const char *tp = prom_bootargs();

	if (!tp || *tp == '\0') {
		*bargs_buf = '\0';
		return;
	}

	SKIP_WHITESPC(tp);

	/*
	 * If we don't have an option indicator, then we
	 * already have our filename prepended.
	 */
	if (*tp && *tp != '-') {
		int i;

		/*
		 * Copy the filename into fname_buf.
		 */
		for (i = 0; i < fname_buf_sz && *tp && !ISSPACE(*tp); ++i)
			*fname_buf++ = *tp++;

		if (i >= fname_buf_sz) {
			printf("boot: boot filename too long!\n");
			printf("boot halted.\n");
			prom_enter_mon();
			/*NOTREACHED*/
		} else {
			*fname_buf = '\0';
		}

		SKIP_WHITESPC(tp);
	}

	/* The rest of the line is the options. */
	while (bargs_buf_sz > 1 && *tp) {
		*bargs_buf++ = *tp++;
		--bargs_buf_sz;
	}
	*bargs_buf = '\0';

	if (bargs_buf_sz == 1) {
		printf("boot: boot arguments too long!\n");
		printf("boot halted.\n");
		prom_enter_mon();
		/*NOTREACHED*/
	}
}

boolean_t
is_netdev(char *devpath)
{
	pnode_t	node = prom_finddevice(devpath);
	char *options;

	if ((node == OBP_NONODE) || (node == OBP_BADNODE))
		return (B_FALSE);
	if (prom_devicetype(node, "network") != 0)
		return (B_TRUE);

	/*
	 * For Infiniband, network device names will be of the
	 * format XXX/ib@0:port=1,pkey=1234,protocol=ip[,YYY] where
	 * XXX is typically /pci@8,700000/pci@1. The device_type
	 * property will be "ib".
	 */
	if (prom_devicetype(node, "ib") != 0) {
		options = (char *)prom_path_options(devpath);
		if (options != NULL) {

#define	SEARCHSTRING	",protocol=ip"
#define	SEARCHSTRLEN	strlen(SEARCHSTRING)

			if (strstr(options, ",protocol=ip,") != NULL)
				return (B_TRUE);
			while ((options = (char *)strstr(options, SEARCHSTRING)) !=
			    NULL) {
				char nextc;

				nextc = options[SEARCHSTRLEN];
				if ((nextc == ',') || (nextc == 0))
					return (B_TRUE);
				options += SEARCHSTRLEN;
			}
		}
	}
	return (B_FALSE);
}

/*
 * Hook for modifying the OS boot path.  This hook allows us to handle
 * device arguments that the OS can't handle.
 */
void
mangle_os_bootpath(char *bpath)
{
	pnode_t node;
	char *stripped_pathname;

	node = prom_finddevice(bpath);
	if (prom_devicetype(node, "network") == 0)
		return;

	/*
	 * The OS can't handle network device arguments
	 * eg: boot net:promiscuous,speed=100,duplex=full
	 * So, we remove any argument strings in the device
	 * pathname we hand off to the OS for network devices.
	 *
	 * Internally, within boot, bpath is used to access
	 * the device, but v2path (as the boot property "boot-path")
	 * is the pathname passed to the OS.
	 */

	stripped_pathname = kmem_alloc(OBP_MAXPATHLEN, 0);
	prom_strip_options(bpath, stripped_pathname);
	v2path = stripped_pathname;
}

/*
 * Given the boot path in the native firmware format use
 * the redirection string to mutate the boot path to the new device.
 * Fix up the 'v2path' so that it matches the new firmware path.
 */
void
redirect_boot_path(char *bpath, char *redirect)
{
	char slicec = *redirect;
	char *p = bpath + strlen(bpath);

	/*
	 * If the redirection character doesn't fall in this
	 * range, something went horribly wrong.
	 */
	if (slicec < '0' || slicec > '7') {
		printf("boot: bad redirection slice '%c'\n", slicec);
		return;
	}

	/*
	 * Handle fully qualified OpenBoot pathname.
	 */
	while (--p >= bpath && *p != '@' && *p != '/')
		if (*p == ':')
			break;
	if (*p++ == ':') {
		/*
		 * Convert slice number to partition 'letter'.
		 */
		*p++ = 'a' + slicec - '0';
		*p = '\0';
		v2path = bpath;
		return;
	}
	prom_panic("redirect_boot_path: mangled boot path!");
}

#define	PROM_VERS_MAX_LEN	64

void
system_check(void)
{
	
	char buf[PROM_VERS_MAX_LEN];
	/*
	if (prom_version_check(buf, PROM_VERS_MAX_LEN, NULL) != PROM_VER64_OK) {
		printf("The firmware on this system does not support the 64-bit"
		    " OS.\n\tPlease upgrade to at least the following version:"
		    "\n\n\t%s\n", buf);
		prom_exit_to_mon();
	}
	*/
}

/*
 * Reads in the standalone (client) program and jumps to it.  If this
 * attempt fails, prints "boot failed" and returns to its caller.
 *
 * It will try to determine if it is loading a Unix file by
 * looking at what should be the magic number.  If it makes
 * sense, it will use it; otherwise it jumps to the first
 * address of the blocks that it reads in.
 *
 * This new boot program will open a file, read the ELF header,
 * attempt to allocate and map memory at the location at which
 * the client desires to be linked, and load the program at
 * that point.  It will then jump there.
 */
/*ARGSUSED*/
int
main(void *cookie, char **argv, int argc)
{
	/*
	 * bpath is the boot device path buffer.
	 * bargs is the boot arguments buffer.
	 */
	static char	bpath[OBP_MAXPATHLEN], bargs[OBP_MAXPATHLEN];
	boolean_t	user_specified_filename;

	prom_init("boot", cookie);
	fiximp();

	system_check();

	dprintf("\nboot: OpenSolaris on PowerPC Boot Interface.\n");
	dprintf("OpenSolaris on PowerPC from Sun Labs and the Blastwave Community!\n");
#ifdef HALTBOOT
	prom_enter_mon();
#endif /* HALTBOOT */

	init_memlists();

#ifdef DEBUG_LISTS
	dprintf("Physmem avail:\n");
	if (debug) print_memlist(pfreelistp);
	dprintf("Virtmem avail:\n");
	if (debug) print_memlist(vfreelistp);
	dprintf("Phys installed:\n");
	if (debug) print_memlist(pinstalledp);
	/* prom_enter_mon(); */
#endif /* DEBUG_LISTS */

	/*
	 * Initialize the default filename (exported as "default-name" and
	 * used by kadb).
	 */
	set_default_filename(defname);

	/*
	 * Parse the arguments ASAP in case there are any flags which may
	 * affect execution.
	 */

	/*
	 * filename is the path to the standalone.  Initialize it to the empty
	 * string so we can tell whether the user specified it in the
	 * arguments.
	 */
	filename[0] = '\0';

	/*
	 * Fetch the boot arguments from the PROM and split the filename off
	 * if it's there.
	 */
	init_bootargs(filename, sizeof (filename), bargs, sizeof (bargs));

	/*
	 * kadb was delivered as a standalone, and as such, people got used to
	 * typing `boot kadb'.  kmdb isn't a standalone - it is loaded by krtld
	 * as just another kernel module.  For compatibility, though, when we
	 * see an attempt to `boot kadb' or `boot kmdb', we'll transform that
	 * into a `boot -k' (or equivalent).
	 */
	if (strcmp(filename, "kmdb") == 0 || strcmp(filename, "kadb") == 0) {
		boothowto |= RB_KMDB;
		*filename = '\0'; /* let boot figure out which unix to use */
	}

	bootflags(bargs, sizeof (bargs));

	user_specified_filename = (filename[0] != '\0');

	/* Fetch the boot path from the PROM. */
	(void) strncpy(bpath, prom_bootpath(), sizeof (bpath) - 1);
	bpath[sizeof (bpath) - 1] = '\0';

	dprintf("bootpath: 0x%p %s\n", (void *)bpath, bpath);
	dprintf("bootargs: 0x%p %s\n", (void *)bargs, bargs);
	dprintf("filename: 0x%p %s\n", (void *)filename, filename);
	dprintf("kernname: 0x%p %s\n", (void *)kernname, kernname);

	/*
	 * *v2path will be exported to the standalone as the boot-path boot
	 * property.
	 */
	v2path = bpath;

	/*
	 * Our memory lists should be "up" by this time
	 */

	setup_bootops();

	/*
	 * If bpath is a network card, set v2path to a copy of bpath with the
	 * options stripped off.
	 */
	mangle_os_bootpath(bpath);

	if (bootprog(bpath, bargs, user_specified_filename) == 0) {
		post_mountroot(filename, NULL);
		/*NOTREACHED*/
	}

	return (0);
}
