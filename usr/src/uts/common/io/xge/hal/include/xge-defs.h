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
 *  Copyright (c) 2002-2005 Neterion, Inc.
 *  All right Reserved.
 *
 *  FileName :    xge-defs.h
 *
 *  Description:  global definitions
 *
 *  Created:      13 May 2004
 */

#ifndef XGE_DEFS_H
#define XGE_DEFS_H

#define XGE_PCI_VENDOR_ID		0x17D5
#define XGE_PCI_DEVICE_ID_XENA_1	0x5731
#define XGE_PCI_DEVICE_ID_XENA_2	0x5831
#define XGE_PCI_DEVICE_ID_HERC_1	0x5732
#define XGE_PCI_DEVICE_ID_HERC_2	0x5832

#define XGE_DRIVER_NAME		"Xge driver"
#define XGE_DRIVER_VENDOR	"Neterion, Inc"
#define XGE_CHIP_FAMILY		"Xframe"
#define XGE_SUPPORTED_MEDIA_0	"Fiber"

#include "version.h"

/*---------------------------- DMA attributes ------------------------------*/
/*           Used in xge_os_dma_malloc() and xge_os_dma_map() */
/*---------------------------- DMA attributes ------------------------------*/

/* XGE_OS_DMA_REQUIRES_SYNC  - should be defined or
                             NOT defined in the Makefile */
#define XGE_OS_DMA_CACHELINE_ALIGNED      0x1
/* Either STREAMING or CONSISTENT should be used.
   The combination of both or none is invalid */
#define XGE_OS_DMA_STREAMING              0x2
#define XGE_OS_DMA_CONSISTENT             0x4
#define XGE_OS_SPRINTF_STRLEN             64

/*---------------------------- common stuffs -------------------------------*/

#ifdef XGE_OS_MEMORY_CHECK
typedef struct {
	void *ptr;
	int size;
	char *file;
	int line;
} xge_os_malloc_t;

#define XGE_OS_MALLOC_CNT_MAX	64*1024
extern xge_os_malloc_t g_malloc_arr[XGE_OS_MALLOC_CNT_MAX];
extern int g_malloc_cnt;

#define XGE_OS_MEMORY_CHECK_MALLOC(_vaddr, _size, _file, _line) { \
	if (_vaddr) { \
		int i; \
		for (i=0; i<g_malloc_cnt; i++) { \
			if (g_malloc_arr[i].ptr == NULL) { \
				break; \
			} \
		} \
		if (i == g_malloc_cnt) { \
			g_malloc_cnt++; \
			if (g_malloc_cnt >= XGE_OS_MALLOC_CNT_MAX) { \
			  xge_os_bug("g_malloc_cnt exceed %d", \
						XGE_OS_MALLOC_CNT_MAX); \
			} \
		} \
		g_malloc_arr[i].ptr = _vaddr; \
		g_malloc_arr[i].size = _size; \
		g_malloc_arr[i].file = _file; \
		g_malloc_arr[i].line = _line; \
		for (i=0; i<_size; i++) { \
			*((char *)_vaddr+i) = 0x5a; \
		} \
	} \
}

#define XGE_OS_MEMORY_CHECK_FREE(_vaddr, _check_size) { \
	int i; \
	for (i=0; i<XGE_OS_MALLOC_CNT_MAX; i++) { \
		if (g_malloc_arr[i].ptr == _vaddr) { \
			g_malloc_arr[i].ptr = NULL; \
			if(_check_size && g_malloc_arr[i].size!=_check_size) { \
				xge_os_printf("OSPAL: freeing with wrong " \
				      "size %d! allocated at %s:%d:%llx:%d", \
					 (int)_check_size, \
					 g_malloc_arr[i].file, \
					 g_malloc_arr[i].line, \
					 (unsigned long long)(ulong_t) \
					    g_malloc_arr[i].ptr, \
					 g_malloc_arr[i].size); \
			} \
			break; \
		} \
	} \
	if (i == XGE_OS_MALLOC_CNT_MAX) { \
		xge_os_printf("OSPAL: ptr %llx not found!", \
			    (unsigned long long)(ulong_t)_vaddr); \
	} \
}
#else
#define XGE_OS_MEMORY_CHECK_MALLOC(ptr, size, file, line)
#define XGE_OS_MEMORY_CHECK_FREE(vaddr, check_size)
#endif

#endif /* XGE_DEFS_H */
