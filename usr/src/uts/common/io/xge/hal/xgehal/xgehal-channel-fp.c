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
 *  FileName :    xgehal-channel-fp.c
 *
 *  Description:  HAL channel object functionality (fast path)
 *
 *  Created:      10 June 2004
 */

#ifdef XGE_DEBUG_FP
#include "xgehal-channel.h"
#endif

__HAL_STATIC_CHANNEL __HAL_INLINE_CHANNEL xge_hal_status_e
__hal_channel_dtr_alloc(xge_hal_channel_h channelh, xge_hal_dtr_h *dtrh)
{
	void **tmp_arr;
	xge_hal_channel_t *channel = (xge_hal_channel_t *)channelh;
	unsigned long flags = 0;

	if (channel->reserve_length - channel->reserve_top >
						channel->reserve_threshold) {

_alloc_after_swap:
		*dtrh = channel->reserve_arr[--channel->reserve_length];

		xge_debug_channel(XGE_TRACE, "dtrh 0x%llx allocated, "
				   "channel %d:%d:%d, reserve_idx %d",
				   (unsigned long long)(ulong_t)*dtrh,
				   channel->type, channel->post_qid,
				   channel->compl_qid, channel->reserve_length);

		return XGE_HAL_OK;
	}

	xge_os_spin_lock_irq(&channel->free_lock, flags);

	/* switch between empty and full arrays */

	/* the idea behind such a design is that by having free and reserved
	 * arrays separated we basically separated irq and non-irq parts.
	 * i.e. no additional lock need to be done when we free a resource */

	if (channel->reserve_initial - channel->free_length >
					channel->reserve_threshold) {

		tmp_arr = channel->reserve_arr;
		channel->reserve_arr = channel->free_arr;
		channel->reserve_length = channel->reserve_initial;
		channel->free_arr = tmp_arr;
		channel->reserve_top = channel->free_length;
		channel->free_length = channel->reserve_initial;

		channel->stats.reserve_free_swaps_cnt++;

		xge_debug_channel(XGE_TRACE,
			   "switch on channel %d:%d:%d, reserve_length %d, "
			   "free_length %d", channel->type, channel->post_qid,
			   channel->compl_qid, channel->reserve_length,
			   channel->free_length);

		xge_os_spin_unlock_irq(&channel->free_lock, flags);

		goto _alloc_after_swap;
	}

	xge_os_spin_unlock_irq(&channel->free_lock, flags);

	xge_debug_channel(XGE_TRACE, "channel %d:%d:%d is empty!",
			   channel->type, channel->post_qid,
			   channel->compl_qid);

	channel->stats.out_of_dtrs_cnt++;

	*dtrh = NULL;
	return XGE_HAL_INF_OUT_OF_DESCRIPTORS;
}

__HAL_STATIC_CHANNEL __HAL_INLINE_CHANNEL void
__hal_channel_dtr_restore(xge_hal_channel_h channelh, xge_hal_dtr_h dtrh, 
			  int offset)
{
	xge_hal_channel_t *channel = (xge_hal_channel_t *)channelh;

	/* restore a previously allocated dtrh at current offset and update
	 * the available reserve length accordingly. If dtrh is null just
	 * update the reserve length, only */

	if (dtrh) {
		channel->reserve_arr[channel->reserve_length + offset] = dtrh;
		xge_debug_channel(XGE_TRACE, "dtrh 0x%llx restored for "
			"channel %d:%d:%d, offset %d at reserve index %d, ",
			(unsigned long long)(ulong_t)dtrh, channel->type,
			channel->post_qid, channel->compl_qid, offset,
			channel->reserve_length + offset);
	}
	else {
		channel->reserve_length += offset;
		xge_debug_channel(XGE_TRACE, "channel %d:%d:%d, restored "
			"for offset %d, new reserve_length %d, free length %d",
			channel->type, channel->post_qid, channel->compl_qid,
			offset, channel->reserve_length, channel->free_length);
	}
}

__HAL_STATIC_CHANNEL __HAL_INLINE_CHANNEL void
__hal_channel_dtr_post(xge_hal_channel_h channelh, xge_hal_dtr_h dtrh)
{
	xge_hal_channel_t *channel    = (xge_hal_channel_t*)channelh;

	xge_assert(channel->work_arr[channel->post_index] == NULL);

	channel->work_arr[channel->post_index++] = dtrh;

        /* wrap-around */
	if (channel->post_index == channel->length)
		channel->post_index = 0;
}

__HAL_STATIC_CHANNEL __HAL_INLINE_CHANNEL void
__hal_channel_dtr_try_complete(xge_hal_channel_h channelh, xge_hal_dtr_h *dtrh)
{
	xge_hal_channel_t *channel = (xge_hal_channel_t *)channelh;

	xge_assert(channel->work_arr);
	xge_assert(channel->compl_index < channel->length);

	*dtrh = channel->work_arr[channel->compl_index];
}

__HAL_STATIC_CHANNEL __HAL_INLINE_CHANNEL void
__hal_channel_dtr_complete(xge_hal_channel_h channelh)
{
	xge_hal_channel_t *channel = (xge_hal_channel_t *)channelh;

	channel->work_arr[channel->compl_index] = NULL;

	/* wrap-around */
	if (++channel->compl_index == channel->length)
		channel->compl_index = 0;

	channel->stats.total_compl_cnt++;
}

__HAL_STATIC_CHANNEL __HAL_INLINE_CHANNEL void
__hal_channel_dtr_free(xge_hal_channel_h channelh, xge_hal_dtr_h dtrh)
{
	xge_hal_channel_t *channel = (xge_hal_channel_t *)channelh;

	channel->free_arr[--channel->free_length] = dtrh;

	xge_debug_channel(XGE_TRACE, "dtrh 0x%llx freed, "
			   "channel %d:%d:%d, new free_length %d",
			   (unsigned long long)(ulong_t)dtrh,
			   channel->type, channel->post_qid,
			   channel->compl_qid, channel->free_length);
}

/**
 * xge_hal_channel_userdata - Get user-specified channel context.
 * @channelh: Channel handle. Obtained via xge_hal_channel_open().
 *
 * Returns: per-channel "user data", which can be any ULD-defined context.
 * The %userdata "gets" into the channel at open time
 * (see xge_hal_channel_open()).
 *
 * See also: xge_hal_channel_open().
 */
__HAL_STATIC_CHANNEL __HAL_INLINE_CHANNEL void*
xge_hal_channel_userdata(xge_hal_channel_h channelh)
{
	xge_hal_channel_t *channel = (xge_hal_channel_t *)channelh;

	return channel->userdata;
}

/**
 * xge_hal_channel_id - Get channel ID.
 * @channelh: Channel handle. Obtained via xge_hal_channel_open().
 *
 * Returns: channel ID. For link layer channel id is the number
 * in the range from 0 to 7 that identifies hardware ring or fifo,
 * depending on the channel type.
 */
__HAL_STATIC_CHANNEL __HAL_INLINE_CHANNEL int
xge_hal_channel_id(xge_hal_channel_h channelh)
{
	xge_hal_channel_t *channel = (xge_hal_channel_t *)channelh;

	return channel->post_qid;
}

/**
 * xge_hal_check_alignment - Check buffer alignment and calculate the
 * "misaligned" portion.
 * @dma_pointer: DMA address of the buffer.
 * @size: Buffer size, in bytes.
 * @alignment: Alignment "granularity" (see below), in bytes.
 * @copy_size: Maximum number of bytes to "extract" from the buffer
 * (in order to spost it as a separate scatter-gather entry). See below.
 *
 * Check buffer alignment and calculate "misaligned" portion, if exists.
 * The buffer is considered aligned if its address is multiple of
 * the specified @alignment. If this is the case,
 * xge_hal_check_alignment() returns zero.
 * Otherwise, xge_hal_check_alignment() uses the last argument,
 * @copy_size,
 * to calculate the size to "extract" from the buffer. The @copy_size
 * may or may not be equal @alignment. The difference between these two
 * arguments is that the @alignment is used to make the decision: aligned
 * or not aligned. While the @copy_size is used to calculate the portion
 * of the buffer to "extract", i.e. to post as a separate entry in the
 * transmit descriptor. For example, the combination
 * @alignment=8 and @copy_size=64 will work okay on AMD Opteron boxes.
 *
 * Note: @copy_size should be a multiple of @alignment. In many practical
 * cases @copy_size and @alignment will probably be equal.
 *
 * See also: xge_hal_fifo_dtr_buffer_set_aligned().
 */
__HAL_STATIC_CHANNEL __HAL_INLINE_CHANNEL int
xge_hal_check_alignment(dma_addr_t dma_pointer, int size, int alignment,
		int copy_size)
{
	int misaligned_size;

	misaligned_size = (int)(dma_pointer & (alignment - 1));
	if (!misaligned_size) {
		return 0;
	}

	if (size > copy_size) {
		misaligned_size = (int)(dma_pointer & (copy_size - 1));
		misaligned_size = copy_size - misaligned_size;
	} else {
		misaligned_size = size;
	}

	return misaligned_size;
}
