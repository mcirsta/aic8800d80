/**
 ******************************************************************************
 *
 * rwnx_fw_trace.h
 *
 * Copyright (C) RivieraWaves 2017-2019
 *
 ******************************************************************************
 */

#ifndef _RWNX_FW_TRACE_H_
#define _RWNX_FW_TRACE_H_

#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define FW_LOG_SIZE (10240)

struct rwnx_fw_log_buf {
	uint8_t *data;
	uint8_t *start;
	uint8_t *end;
	uint8_t *dataend;
	uint32_t size;
};

struct rwnx_fw_log {
	struct rwnx_fw_log_buf buf;
	spinlock_t lock;
};

int rwnx_fw_log_init(struct rwnx_fw_log *fw_log, bool enabled);
size_t rwnx_fw_log_append(struct rwnx_fw_log *fw_log, const u8 *msg, size_t len);
size_t rwnx_fw_log_read(struct rwnx_fw_log *fw_log, u8 *data, size_t count);
void rwnx_fw_log_deinit(struct rwnx_fw_log *fw_log);
#endif /* _RWNX_FW_TRACE_H_ */
