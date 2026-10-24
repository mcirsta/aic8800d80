/**
 ******************************************************************************
 *
 * @file rwnx_fw_trace.c
 *
 * Copyright (C) RivieraWaves 2017-2019
 *
 ******************************************************************************
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include "rwnx_fw_trace.h"

/* Initialize once, before publishing the enclosing hw to RX or debugfs. */
int rwnx_fw_log_init(struct rwnx_fw_log *fw_log, bool enabled)
{
	u8 *data;

	spin_lock_init(&fw_log->lock);
	memset(&fw_log->buf, 0, sizeof(fw_log->buf));
	if (!enabled)
		return 0;
	data = kmalloc(FW_LOG_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	spin_lock_bh(&fw_log->lock);
	fw_log->buf.start = data;
	fw_log->buf.end = data;
	fw_log->buf.dataend = data + FW_LOG_SIZE;
	fw_log->buf.data = data;
	spin_unlock_bh(&fw_log->lock);
	return 0;
}

void rwnx_fw_log_deinit(struct rwnx_fw_log *fw_log)
{
	u8 *data;

	/* The lock survives until the enclosing hw is released. */
	spin_lock_bh(&fw_log->lock);
	data = fw_log->buf.data;
	memset(&fw_log->buf, 0, sizeof(fw_log->buf));
	spin_unlock_bh(&fw_log->lock);
	kfree(data);
}

size_t rwnx_fw_log_append(struct rwnx_fw_log *fw_log, const u8 *msg, size_t len)
{
	struct rwnx_fw_log_buf *buf = &fw_log->buf;
	size_t first, dropped;

	if (!msg || !len)
		return 0;
	if (len > FW_LOG_SIZE) {
		msg += len - FW_LOG_SIZE;
		len = FW_LOG_SIZE;
	}
	spin_lock_bh(&fw_log->lock);
	if (!buf->data) {
		spin_unlock_bh(&fw_log->lock);
		return 0;
	}
	/* Retain the newest bytes, advancing the oldest unread byte on overflow. */
	dropped = max_t(size_t, buf->size + len, FW_LOG_SIZE) - FW_LOG_SIZE;
	buf->start = buf->data + ((buf->start - buf->data + dropped) % FW_LOG_SIZE);
	first = min_t(size_t, len, buf->dataend - buf->end);
	memcpy(buf->end, msg, first);
	memcpy(buf->data, msg + first, len - first);
	buf->end = buf->data + ((buf->end - buf->data + len) % FW_LOG_SIZE);
	buf->size = min_t(size_t, buf->size + len, FW_LOG_SIZE);
	spin_unlock_bh(&fw_log->lock);
	return len;
}

/* Consume a coherent bounded snapshot; the caller owns its destination. */
size_t rwnx_fw_log_read(struct rwnx_fw_log *fw_log, u8 *data, size_t count)
{
	struct rwnx_fw_log_buf *buf = &fw_log->buf;
	size_t len, first;

	if (!count)
		return 0;
	spin_lock_bh(&fw_log->lock);
	if (!buf->data) {
		spin_unlock_bh(&fw_log->lock);
		return 0;
	}
	len = min_t(size_t, count, buf->size);
	first = min_t(size_t, len, buf->dataend - buf->start);
	memcpy(data, buf->start, first);
	memcpy(data + first, buf->data, len - first);
	buf->start = buf->data + ((buf->start - buf->data + len) % FW_LOG_SIZE);
	buf->size -= len;
	spin_unlock_bh(&fw_log->lock);
	return len;
}
