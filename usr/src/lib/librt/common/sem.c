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
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)sem.c	1.26	06/01/04 SMI"

#pragma weak	sem_open = _sem_open
#pragma weak	sem_close = _sem_close
#pragma weak	sem_unlink = _sem_unlink
#pragma weak	sem_init = _sem_init
#pragma weak	sem_destroy = _sem_destroy
#pragma weak	sem_wait = _sem_wait
#pragma weak	sem_timedwait = _sem_timedwait
#pragma weak	sem_reltimedwait_np = _sem_reltimedwait_np
#pragma weak	sem_trywait = _sem_trywait
#pragma weak	sem_post = _sem_post
#pragma weak	sem_getvalue = _sem_getvalue

#include "c_synonyms.h"
#include <sys/types.h>
#include <semaphore.h>
#include <synch.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread.h>
#include "pos4obj.h"
#include "pos4.h"

typedef	struct	semaddr {
	struct	semaddr	*sad_next;	/* next in the link */
	char		sad_name[PATH_MAX + 1]; /* name of sem object */
	sem_t		*sad_addr;	/* mmapped address of semaphore */
	ino64_t		sad_inode;	/* inode # of the mmapped file */
} semaddr_t;

static	semaddr_t	*semheadp = NULL;
static	mutex_t		semlock = DEFAULTMUTEX;

sem_t *
_sem_open(const char *path, int oflag, /* mode_t mode, int value */ ...)
{
	va_list	ap;
	mode_t	crmode = 0;
	sem_t	*sem = NULL;
	struct	stat64 statbuf;
	semaddr_t *next = NULL;
	int	fd = 0;
	int	error = 0;
	int	cr_flag = 0;
	uint_t	value = 0;

	if (__pos4obj_check(path) == -1)
		return (SEM_FAILED);

	/* acquire semaphore lock to have atomic operation */
	if (__pos4obj_lock(path, SEM_LOCK_TYPE) < 0)
		return (SEM_FAILED);

	/* modify oflag to have RDWR and filter CREATE mode only */
	oflag = (oflag & (O_CREAT|O_EXCL)) | (O_RDWR);
	if ((oflag & O_CREAT) != 0) {
		va_start(ap, oflag);
		crmode = va_arg(ap, mode_t);
		value = va_arg(ap, uint_t);
		va_end(ap);
		/* check value < the max for a named semaphore */
		if ((_lsemvaluemax == -1L) ||
		    ((unsigned long)value > (unsigned long)_lsemvaluemax)) {
			errno = EINVAL;
			goto out;
		}
	}

	errno = 0;

	if ((fd = __pos4obj_open(path, SEM_DATA_TYPE,
				oflag, crmode, &cr_flag)) < 0)
		goto out;

	if (cr_flag)
		cr_flag = DFILE_CREATE | DFILE_OPEN;
	else
		cr_flag = DFILE_OPEN;

	/* find out inode # for the opened file */
	if (fstat64(fd, &statbuf) < 0)
		goto out;

	/* if created, acquire total_size in the file */
	if ((cr_flag & DFILE_CREATE) != 0) {
		if (ftruncate64(fd, (off64_t)sizeof (sem_t)) < 0)
			goto out;

	} else {
		(void) mutex_lock(&semlock);

		/*
		 * if this semaphore has already been opened, inode
		 * will indicate then return the same semaphore address
		 */
		for (next = semheadp; next != NULL; next = next->sad_next) {
			if (statbuf.st_ino == next->sad_inode &&
				strcmp(path, next->sad_name) == 0) {

				(void) __close_nc(fd);
				(void) mutex_unlock(&semlock);
				(void) __pos4obj_unlock(path, SEM_LOCK_TYPE);
				return ((sem_t *)next->sad_addr);
			}
		}
		(void) mutex_unlock(&semlock);
	}


	/* new sem descriptor to be allocated and new address to be mapped */
	if ((next = (semaddr_t *)malloc(sizeof (semaddr_t))) == NULL) {
		errno = ENOMEM;
		goto out;
	}

	cr_flag |= ALLOC_MEM;

	/* LINTED */
	sem = (sem_t *)mmap64(NULL, sizeof (sem_t), PROT_READ|PROT_WRITE,
				MAP_SHARED, fd, (off64_t)0);
	(void) __close_nc(fd);
	cr_flag &= ~DFILE_OPEN;

	if (sem == MAP_FAILED)
		goto out;

	cr_flag |= DFILE_MMAP;

	/* add to the list pointed by semheadp */
	next->sad_next = semheadp;
	semheadp = next;
	next->sad_addr = sem;
	next->sad_inode = statbuf.st_ino;
	(void) strcpy(next->sad_name, path);

	/* initialize it by jumping through the jump table */
	if ((cr_flag & DFILE_CREATE) != 0) {
		if ((error = sema_init((sema_t *)sem, value, USYNC_PROCESS, 0))
		    != 0) {
			errno = error;
			goto out;
		}
	}

	if (__pos4obj_unlock(path, SEM_LOCK_TYPE) < 0)
		return (SEM_FAILED);

	return (sem);

out:
	error = errno;
	if ((cr_flag & DFILE_OPEN) != 0)
		(void) __close_nc(fd);
	if ((cr_flag & DFILE_CREATE) != 0)
		(void) __pos4obj_unlink(path, SEM_DATA_TYPE);
	if ((cr_flag & ALLOC_MEM) != 0)
		free((caddr_t)next);
	if ((cr_flag & DFILE_MMAP) != 0)
		(void) munmap((caddr_t)sem, sizeof (sem_t));

	errno = error;
	(void) __pos4obj_unlock(path, SEM_LOCK_TYPE);
	return (SEM_FAILED);
}

int
_sem_close(sem_t *sem)
{
	semaddr_t	**next;
	semaddr_t	*freeit;

	(void) mutex_lock(&semlock);

	for (next = &semheadp; (freeit = *next) != NULL;
	    next = &(freeit->sad_next)) {
		if (freeit->sad_addr == sem) {
			*next = freeit->sad_next;
			free((caddr_t)freeit);
			(void) mutex_unlock(&semlock);
			return (munmap((caddr_t)sem, sizeof (sem_t)));
		}
	}
	(void) mutex_unlock(&semlock);
	errno = EINVAL;
	return (-1);
}

int
_sem_unlink(const char *path)
{
	int	error;
	int	oerrno;

	if (__pos4obj_check(path) < 0)
		return (-1);

	if (__pos4obj_lock(path, SEM_LOCK_TYPE) < 0)
		return (-1);

	error =  __pos4obj_unlink(path, SEM_DATA_TYPE);

	oerrno = errno;

	(void) __pos4obj_unlock(path, SEM_LOCK_TYPE);

	errno = oerrno;

	return (error);
}

/*
 * SUSV3 requires ("shall fail") an EINVAL failure for operations
 * on invalid semaphores, including uninitialized unnamed semaphores.
 * The best we can do is check that the magic number is correct.
 * This is not perfect, but it allows the test suite to pass.
 * (Standards bodies are filled with fools and idiots.)
 */
static int
sem_invalid(sem_t *sem)
{
	if (sem->sem_magic != SEMA_MAGIC) {
		errno = EINVAL;
		return (-1);
	}
	return (0);
}

int
_sem_init(sem_t *sem, int pshared, uint_t value)
{
	int	error;

	if ((error = sema_init((sema_t *)sem, value,
	    pshared ? USYNC_PROCESS : USYNC_THREAD, NULL)) != 0) {
		errno = error;
		return (-1);
	}
	return (0);
}

int
_sem_destroy(sem_t *sem)
{
	int	error;

	if (sem_invalid(sem))
		return (-1);
	if ((error = sema_destroy((sema_t *)sem)) != 0) {
		errno = error;
		return (-1);
	}
	return (0);
}

int
_sem_post(sem_t *sem)
{
	int	error;

	if (sem_invalid(sem))
		return (-1);
	if ((error = sema_post((sema_t *)sem)) != 0) {
		errno = error;
		return (-1);
	}
	return (0);
}

int
_sem_wait(sem_t *sem)
{
	int	error;

	if (sem_invalid(sem))
		return (-1);
	if ((error = sema_wait((sema_t *)sem)) != 0) {
		errno = error;
		return (-1);
	}
	return (0);
}

int
_sem_timedwait(sem_t *sem, const timespec_t *abstime)
{
	int	error;

	if (sem_invalid(sem))
		return (-1);
	if ((error = sema_timedwait((sema_t *)sem, abstime)) != 0) {
		if (error == ETIME)
			error = ETIMEDOUT;
		errno = error;
		return (-1);
	}
	return (0);
}

int
_sem_reltimedwait_np(sem_t *sem, const timespec_t *reltime)
{
	int	error;

	if (sem_invalid(sem))
		return (-1);
	if ((error = sema_reltimedwait((sema_t *)sem, reltime)) != 0) {
		if (error == ETIME)
			error = ETIMEDOUT;
		errno = error;
		return (-1);
	}
	return (0);
}

int
_sem_trywait(sem_t *sem)
{
	int	error;

	if (sem_invalid(sem))
		return (-1);
	if ((error = sema_trywait((sema_t *)sem)) != 0) {
		if (error == EBUSY)
			error = EAGAIN;
		errno = error;
		return (-1);
	}
	return (0);
}

int
_sem_getvalue(sem_t *sem, int *sval)
{
	if (sem_invalid(sem))
		return (-1);
	*sval = (int)sem->sem_count;
	return (0);
}
