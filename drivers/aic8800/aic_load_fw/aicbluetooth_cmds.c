/**
 ******************************************************************************
 *
 * rwnx_cmds.c
 *
 * Handles queueing (push to IPC, ack/cfm from IPC) of commands issued to
 * LMAC FW
 *
 * Copyright (C) RivieraWaves 2014-2019
 *
 ******************************************************************************
 */

#include <linux/list.h>
//#include <stddef.h>
#include "aicbluetooth_cmds.h"
#include "aic_txrxif.h"
#include "aicwf_debug.h"
#include "aicwf_usb.h"

//extern int aicwf_sdio_writeb(struct aic_sdio_dev *sdiodev, uint regaddr, u8 val);

static void cmd_dump(const struct rwnx_cmd *cmd)
{
    AICWFDBG(LOGDEBUG, "tkn[%d]  flags:%04x  result:%3d  cmd:%4d - reqcfm(%4d)\n",
           cmd->tkn, cmd->flags, cmd->result, cmd->id, cmd->reqid);
}

/* The sender owns cmd and its message; the queue only borrows cmd. */
static void cmd_complete(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd,
			 int result)
{
	lockdep_assert_held(&cmd_mgr->lock);

	list_del_init(&cmd->list);
	cmd_mgr->queue_sz--;
	cmd->result = result;
	cmd->flags |= RWNX_CMD_FLAG_DONE;
	complete(&cmd->complete);
}

static int cmd_mgr_queue(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
	long waited;
	int ret;

	lockdep_assert_held(&cmd_mgr->tx_lock);
	/* All current loader requests are synchronous; there is no push worker. */
	if (cmd->flags & RWNX_CMD_FLAG_NONBLOCK)
		return -EOPNOTSUPP;

	spin_lock_bh(&cmd_mgr->lock);
	if (cmd_mgr->state != RWNX_CMD_MGR_STATE_INITED) {
		ret = cmd_mgr->state == RWNX_CMD_MGR_STATE_CRASHED ?
			-EPIPE : -ESHUTDOWN;
		goto out_unlock;
	}
	if (cmd_mgr->queue_sz >= cmd_mgr->max_queue_sz) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	if (cmd->flags & RWNX_CMD_FLAG_REQ_CFM)
		cmd->flags |= RWNX_CMD_FLAG_WAIT_CFM;
	cmd->tkn = cmd_mgr->next_tkn++;
	cmd->result = -EINPROGRESS;
	init_completion(&cmd->complete);
	list_add_tail(&cmd->list, &cmd_mgr->cmds);
	cmd_mgr->queue_sz++;
	spin_unlock_bh(&cmd_mgr->lock);

	ret = aicwf_set_cmd_tx(cmd_mgr->usbdev, cmd->a2e_msg,
			      RWNX_CMD_A2EMSG_LEN(cmd->a2e_msg));
	if (ret) {
		spin_lock_bh(&cmd_mgr->lock);
		/* A late confirmation cannot be correlated after a failed transfer. */
		if (cmd_mgr->state == RWNX_CMD_MGR_STATE_INITED)
			cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
		if (!(cmd->flags & RWNX_CMD_FLAG_DONE))
			cmd_complete(cmd_mgr, cmd, ret);
		else
			cmd->result = ret;
		goto out_unlock;
	}

	waited = wait_for_completion_killable_timeout(&cmd->complete,
				msecs_to_jiffies(RWNX_80211_CMD_TIMEOUT_MS));
	spin_lock_bh(&cmd_mgr->lock);
	if (!(cmd->flags & RWNX_CMD_FLAG_DONE)) {
		ret = waited < 0 ? waited : -ETIMEDOUT;
		/* The ABI matches replies by ID, so do not reuse it after abandonment. */
		if (cmd_mgr->state == RWNX_CMD_MGR_STATE_INITED)
			cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
		cmd_complete(cmd_mgr, cmd, ret);
	}
	/* A confirmation that won the lock at the deadline keeps its result. */
	ret = cmd->result;
out_unlock:
	spin_unlock_bh(&cmd_mgr->lock);
	return ret;
}

static int cmd_mgr_run_callback(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd,
                                struct rwnx_cmd_e2amsg *msg, msg_cb_fct cb)
{
    int res;

    if (! cb){
        return 0;
    }
    spin_lock(&cmd_mgr->cb_lock);
    res = cb(cmd, msg);
    spin_unlock(&cmd_mgr->cb_lock);

    return res;
}

static int cmd_mgr_msgind(struct rwnx_cmd_mgr *cmd_mgr,
			 struct rwnx_cmd_e2amsg *msg, size_t msg_len,
			 msg_cb_fct cb)
{
	struct rwnx_cmd *cmd;
	bool found = false;
	int ret = 0;
	size_t hdr_len = offsetof(struct rwnx_cmd_e2amsg, param);

	if (!msg || msg_len < hdr_len)
		return -EMSGSIZE;
	if (msg->param_len > sizeof(msg->param) ||
	    msg->param_len > msg_len - hdr_len)
		ret = -EMSGSIZE;
	spin_lock(&cmd_mgr->lock);
	if (cmd_mgr->state != RWNX_CMD_MGR_STATE_INITED) {
		spin_unlock(&cmd_mgr->lock);
		return 0;
	}
	list_for_each_entry(cmd, &cmd_mgr->cmds, list) {
		if (cmd->reqid == msg->id &&
			(cmd->flags & RWNX_CMD_FLAG_WAIT_CFM)) {
			if (ret || (cmd->e2a_msg &&
				    msg->param_len != cmd->e2a_msg_len)) {
				/* A malformed reply cannot authorize reuse of its ID. */
				cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
				cmd_complete(cmd_mgr, cmd, -EMSGSIZE);
				ret = -EMSGSIZE;
				found = true;
				break;
			}

			if (!cmd_mgr_run_callback(cmd_mgr, cmd, msg, cb)) {
				found = true;
				cmd->flags &= ~RWNX_CMD_FLAG_WAIT_CFM;

				if (cmd->e2a_msg && msg->param_len)
					memcpy(cmd->e2a_msg, &msg->param, msg->param_len);

				if (RWNX_CMD_WAIT_COMPLETE(cmd->flags))
					cmd_complete(cmd_mgr, cmd, 0);

				break;
			}
		}
	}
	spin_unlock(&cmd_mgr->lock);

	if (!found && !ret)
		cmd_mgr_run_callback(cmd_mgr, NULL, msg, cb);

	return ret;
}

static void cmd_mgr_print(struct rwnx_cmd_mgr *cmd_mgr)
{
    struct rwnx_cmd *cur;

    spin_lock_bh(&cmd_mgr->lock);
    AICWFDBG(LOGDEBUG, "q_sz/max: %2d / %2d - next tkn: %d\n",
             cmd_mgr->queue_sz, cmd_mgr->max_queue_sz,
             cmd_mgr->next_tkn);
    list_for_each_entry(cur, &cmd_mgr->cmds, list) {
        cmd_dump(cur);
    }
    spin_unlock_bh(&cmd_mgr->lock);
}

static void cmd_mgr_drain(struct rwnx_cmd_mgr *cmd_mgr)
{
	struct rwnx_cmd *cmd, *next;

	spin_lock_bh(&cmd_mgr->lock);
	cmd_mgr->state = RWNX_CMD_MGR_STATE_DEINIT;
	list_for_each_entry_safe(cmd, next, &cmd_mgr->cmds, list)
		cmd_complete(cmd_mgr, cmd, -ESHUTDOWN);
	spin_unlock_bh(&cmd_mgr->lock);
}

void rwnx_cmd_mgr_init(struct rwnx_cmd_mgr *cmd_mgr)
{
	mutex_init(&cmd_mgr->tx_lock);
	cmd_mgr->queue_sz = 0;
	cmd_mgr->next_tkn = 0;
    cmd_mgr->max_queue_sz = RWNX_CMD_MAX_QUEUED;
    INIT_LIST_HEAD(&cmd_mgr->cmds);
    cmd_mgr->state = RWNX_CMD_MGR_STATE_INITED;
    spin_lock_init(&cmd_mgr->lock);
    spin_lock_init(&cmd_mgr->cb_lock);
    cmd_mgr->queue  = &cmd_mgr_queue;
    cmd_mgr->print  = &cmd_mgr_print;
    cmd_mgr->drain  = &cmd_mgr_drain;
    cmd_mgr->llind  = NULL;//&cmd_mgr_llind;
    cmd_mgr->msgind = &cmd_mgr_msgind;

    #if 0
    INIT_WORK(&cmd_mgr->cmdWork, cmd_mgr_task_process);
    cmd_mgr->cmd_wq = create_singlethread_workqueue("cmd_wq");
    if (!cmd_mgr->cmd_wq) {
        txrx_err("insufficient memory to create cmd workqueue.\n");
        return;
    }
    #endif
}

void rwnx_cmd_mgr_stop(struct rwnx_cmd_mgr *cmd_mgr)
{
	/* Wake a pending sender before waiting for its transmit-buffer ownership. */
	cmd_mgr_drain(cmd_mgr);
	mutex_lock(&cmd_mgr->tx_lock);
	mutex_unlock(&cmd_mgr->tx_lock);
}

void rwnx_cmd_mgr_deinit(struct rwnx_cmd_mgr *cmd_mgr)
{
	/* stop is idempotent; bus teardown already stops it before freeing buffers. */
	rwnx_cmd_mgr_stop(cmd_mgr);
}

int aicwf_set_cmd_tx(void *dev, struct lmac_msg *msg, uint len)
{
	struct aic_usb_dev *usbdev = dev;
	struct aicwf_bus *bus = usbdev->bus_if;
	u8 *buffer = bus->cmd_buf;
	u16 index = 8;

	lockdep_assert_held(&usbdev->cmd_mgr.tx_lock);
	if (READ_ONCE(bus->state) == BUS_DOWN_ST)
		return -ENETDOWN;
	if (!buffer)
		return -ESHUTDOWN;
	if (len != RWNX_CMD_A2EMSG_LEN(msg))
		return -EINVAL;
	if (len > CMD_BUF_MAX - 8)
		return -EMSGSIZE;

	memset(buffer, 0, CMD_BUF_MAX);
	buffer[0] = (len + 4) & 0xff;
	buffer[1] = ((len + 4) >> 8) & 0x0f;
	buffer[2] = 0x11;
	/* Bytes 4..7 are the firmware's dummy word. */
	put_u16(&buffer[index], msg->id);
	index += 2;
	put_u16(&buffer[index], msg->dest_id);
	index += 2;
	put_u16(&buffer[index], msg->src_id);
	index += 2;
	put_u16(&buffer[index], msg->param_len);
	index += 2;
	memcpy(&buffer[index], msg->param, msg->param_len);

	return aicwf_bus_txmsg(bus, buffer, len + 8);
}

static inline void *rwnx_msg_zalloc(lmac_msg_id_t const id,
                                    lmac_task_id_t const dest_id,
                                    lmac_task_id_t const src_id,
                                    uint16_t const param_len)
{
    struct lmac_msg *msg;
    gfp_t flags;

    if (in_softirq())
        flags = GFP_ATOMIC;
    else
        flags = GFP_KERNEL;

    msg = (struct lmac_msg *)kzalloc(sizeof(struct lmac_msg) + param_len,
                                     flags);
    if (msg == NULL) {
        pr_err("%s: msg allocation failed\n", __func__);
        return NULL;
    }
    msg->id = id;
    msg->dest_id = dest_id;
    msg->src_id = src_id;
    msg->param_len = param_len;

    return msg->param;
}

static void rwnx_msg_free(struct lmac_msg *msg, const void *msg_params)
{
    kfree(msg);
}


static int rwnx_send_msg(struct aic_usb_dev *usbdev, const void *msg_params,
			 int reqcfm, lmac_msg_id_t reqid, void *cfm,
			 size_t cfm_len)
{
	struct rwnx_cmd_mgr *cmd_mgr = &usbdev->cmd_mgr;
	struct lmac_msg *msg;
	struct rwnx_cmd *cmd;
	int ret;

	msg = container_of((void *)msg_params, struct lmac_msg, param);
	if ((!cfm && cfm_len) || (cfm && !cfm_len) ||
	    (!reqcfm && cfm_len)) {
		ret = -EINVAL;
		goto out_free_msg;
	}
	ret = mutex_lock_interruptible(&cmd_mgr->tx_lock);
	if (ret)
		goto out_free_msg;

	if (READ_ONCE(usbdev->bus_if->state) == BUS_DOWN_ST) {
		ret = -ENETDOWN;
		goto out_unlock;
	}
	spin_lock_bh(&cmd_mgr->lock);
	if (cmd_mgr->state != RWNX_CMD_MGR_STATE_INITED)
		ret = cmd_mgr->state == RWNX_CMD_MGR_STATE_CRASHED ?
			-EPIPE : -ESHUTDOWN;
	spin_unlock_bh(&cmd_mgr->lock);
	if (ret)
		goto out_unlock;

	if (!reqcfm) {
		ret = aicwf_set_cmd_tx(usbdev, msg, RWNX_CMD_A2EMSG_LEN(msg));
		if (ret) {
			spin_lock_bh(&cmd_mgr->lock);
			if (cmd_mgr->state == RWNX_CMD_MGR_STATE_INITED)
				cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
			spin_unlock_bh(&cmd_mgr->lock);
		}
		goto out_unlock;
	}

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	cmd->id = msg->id;
	cmd->reqid = reqid;
	cmd->a2e_msg = msg;
	cmd->e2a_msg = cfm;
	cmd->e2a_msg_len = cfm_len;
	cmd->flags = RWNX_CMD_FLAG_REQ_CFM;
	ret = cmd_mgr->queue(cmd_mgr, cmd);
	kfree(cmd);
out_unlock:
	mutex_unlock(&cmd_mgr->tx_lock);
out_free_msg:
	rwnx_msg_free(msg, msg_params);
	return ret;
}

int rwnx_send_dbg_mem_mask_write_req(struct aic_usb_dev *usbdev, u32 mem_addr,
                                     u32 mem_mask, u32 mem_data)
{
    struct dbg_mem_mask_write_req *mem_mask_write_req;

    /* Build the DBG_MEM_MASK_WRITE_REQ message */
    mem_mask_write_req = rwnx_msg_zalloc(DBG_MEM_MASK_WRITE_REQ, TASK_DBG, DRV_TASK_ID,
                                         sizeof(struct dbg_mem_mask_write_req));
    if (!mem_mask_write_req)
        return -ENOMEM;

    /* Set parameters for the DBG_MEM_MASK_WRITE_REQ message */
    mem_mask_write_req->memaddr = mem_addr;
    mem_mask_write_req->memmask = mem_mask;
    mem_mask_write_req->memdata = mem_data;

    /* Send the DBG_MEM_MASK_WRITE_REQ message to LMAC FW */
	return rwnx_send_msg(usbdev, mem_mask_write_req, 1, DBG_MEM_MASK_WRITE_CFM,
			     NULL, 0);
}



int rwnx_send_dbg_mem_block_write_req(struct aic_usb_dev *usbdev, u32 mem_addr,
                                      u32 mem_size, u32 *mem_data)
{
    struct dbg_mem_block_write_req *mem_blk_write_req;

    /* Build the DBG_MEM_BLOCK_WRITE_REQ message */
    mem_blk_write_req = rwnx_msg_zalloc(DBG_MEM_BLOCK_WRITE_REQ, TASK_DBG, DRV_TASK_ID,
                                        sizeof(struct dbg_mem_block_write_req));
    if (!mem_blk_write_req)
        return -ENOMEM;

    /* Set parameters for the DBG_MEM_BLOCK_WRITE_REQ message */
    mem_blk_write_req->memaddr = mem_addr;
    mem_blk_write_req->memsize = mem_size;
    memcpy(mem_blk_write_req->memdata, mem_data, mem_size);

    /* Send the DBG_MEM_BLOCK_WRITE_REQ message to LMAC FW */
	return rwnx_send_msg(usbdev, mem_blk_write_req, 1, DBG_MEM_BLOCK_WRITE_CFM,
			     NULL, 0);
}


int rwnx_send_dbg_mem_read_req(struct aic_usb_dev *usbdev, u32 mem_addr,
                               struct dbg_mem_read_cfm *cfm)
{
    struct dbg_mem_read_req *mem_read_req;

	if (!cfm)
		return -EINVAL;

    /* Build the DBG_MEM_READ_REQ message */
    mem_read_req = rwnx_msg_zalloc(DBG_MEM_READ_REQ, TASK_DBG, DRV_TASK_ID,
                                   sizeof(struct dbg_mem_read_req));
    if (!mem_read_req)
        return -ENOMEM;

    /* Set parameters for the DBG_MEM_READ_REQ message */
    mem_read_req->memaddr = mem_addr;

    /* Send the DBG_MEM_READ_REQ message to LMAC FW */
	return rwnx_send_msg(usbdev, mem_read_req, 1, DBG_MEM_READ_CFM, cfm,
			     sizeof(*cfm));
}


int rwnx_send_dbg_mem_write_req(struct aic_usb_dev *usbdev, u32 mem_addr, u32 mem_data)
{
    struct dbg_mem_write_req *mem_write_req;

	//printk("%s mem_addr:%x mem_data:%x\r\n", __func__, mem_addr, mem_data);

    /* Build the DBG_MEM_WRITE_REQ message */
    mem_write_req = rwnx_msg_zalloc(DBG_MEM_WRITE_REQ, TASK_DBG, DRV_TASK_ID,
                                    sizeof(struct dbg_mem_write_req));
    if (!mem_write_req)
        return -ENOMEM;

    /* Set parameters for the DBG_MEM_WRITE_REQ message */
    mem_write_req->memaddr = mem_addr;
    mem_write_req->memdata = mem_data;

    /* Send the DBG_MEM_WRITE_REQ message to LMAC FW */
	return rwnx_send_msg(usbdev, mem_write_req, 1, DBG_MEM_WRITE_CFM, NULL, 0);
}

int rwnx_send_dbg_start_app_req(struct aic_usb_dev *usbdev, u32 boot_addr,
                                u32 boot_type)
{
    struct dbg_start_app_req *start_app_req;


    /* Build the DBG_START_APP_REQ message */
    start_app_req = rwnx_msg_zalloc(DBG_START_APP_REQ, TASK_DBG, DRV_TASK_ID,
                                    sizeof(struct dbg_start_app_req));
    if (!start_app_req)
        return -ENOMEM;

    /* Set parameters for the DBG_START_APP_REQ message */
    start_app_req->bootaddr = boot_addr;
    start_app_req->boottype = boot_type;

    /* Send the DBG_START_APP_REQ message to LMAC FW */
	return rwnx_send_msg(usbdev, start_app_req, 0, 0, NULL, 0);
}

static msg_cb_fct dbg_hdlrs[MSG_I(DBG_MAX)] = {
};

int rwnx_rx_handle_msg(struct aic_usb_dev *usbdev, struct ipc_e2a_msg *msg,
		       size_t msg_len)
{
	if (!msg || msg_len < offsetof(struct ipc_e2a_msg, param))
		return -EMSGSIZE;
	/* The loader supports only the debug task; empty callbacks are valid. */
	if (MSG_T(msg->id) != TASK_DBG ||
	    MSG_I(msg->id) >= ARRAY_SIZE(dbg_hdlrs))
		return -EOPNOTSUPP;
	return usbdev->cmd_mgr.msgind(&usbdev->cmd_mgr, msg, msg_len,
				     dbg_hdlrs[MSG_I(msg->id)]);
}


int rwnx_send_reboot(struct aic_usb_dev *usbdev)
{
    int ret = 0;
    u32 delay = 2 *1000; //1s

    AICWFDBG(LOGDEBUG, "%s\n", __func__);

    ret = rwnx_send_dbg_start_app_req(usbdev, delay, HOST_START_APP_REBOOT);
    return ret;
}



