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

//#define CREATE_TRACE_POINTS
#include <linux/list.h>

#include "rwnx_cmds.h"
#include "rwnx_msg_tx.h"
#include "rwnx_defs.h"
#include "rwnx_strs.h"
#include "rwnx_events.h"
#include "aicwf_txrxif.h"
#include "rwnx_wakelock.h"
#ifdef AICWF_SDIO_SUPPORT
#include "aicwf_sdio.h"
#else
#include "aicwf_usb.h"
#endif

/* Caller holds the manager lock; terminal ownership is transferred once. */
static void cmd_complete(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd,
			 int result)
{
	lockdep_assert_held(&cmd_mgr->lock);
	if (cmd->flags & RWNX_CMD_FLAG_DONE)
		return;

	list_del_init(&cmd->list);
	cmd_mgr->queue_sz--;
	cmd->flags &= ~(RWNX_CMD_FLAG_WAIT_PUSH | RWNX_CMD_FLAG_WAIT_ACK |
			RWNX_CMD_FLAG_WAIT_CFM);
	cmd->flags |= RWNX_CMD_FLAG_DONE;
	cmd->result = result;
	/* No callback may borrow the caller's response after terminal completion. */
	cmd->e2a_msg = NULL;
	if (!cmd_mgr->queue_sz && cmd_mgr->hw)
		rwnx_wakeup_unlock(cmd_mgr->hw->ws_tx);
	complete_all(&cmd->complete);
	rwnx_cmd_free(cmd);
}

static void cmd_mgr_abort(struct rwnx_cmd_mgr *cmd_mgr,
			  struct rwnx_cmd *failed, int result)
{
	struct rwnx_cmd *cmd, *next;
#ifdef AICWF_USB_SUPPORT
	bool recover = failed && cmd_mgr->state == RWNX_CMD_MGR_STATE_INITED;
#endif

	lockdep_assert_held(&cmd_mgr->lock);
	cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
	list_for_each_entry_safe(cmd, next, &cmd_mgr->cmds, list)
		cmd_complete(cmd_mgr, cmd, !failed || cmd == failed ? result : -EPIPE);
#ifdef AICWF_USB_SUPPORT
	if (recover)
		aicwf_usb_recovery_request(container_of(cmd_mgr, struct aic_usb_dev, cmd_mgr),
					   result);
#endif
}

static void cmd_mgr_user_put(struct rwnx_cmd_mgr *cmd_mgr)
{
	spin_lock_bh(&cmd_mgr->lock);
	if (!--cmd_mgr->users)
		complete_all(&cmd_mgr->users_idle);
	spin_unlock_bh(&cmd_mgr->lock);
}

/* Consume the incoming reference on every return, including admission errors. */
static int cmd_mgr_queue(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
	bool async = cmd->flags & RWNX_CMD_FLAG_NONBLOCK;
	int ret;

	spin_lock_bh(&cmd_mgr->lock);
	if (cmd_mgr->state != RWNX_CMD_MGR_STATE_INITED || !cmd_mgr->cmd_wq) {
		ret = -EPIPE;
		goto reject;
	}
	if (cmd_mgr->queue_sz >= cmd_mgr->max_queue_sz) {
		ret = -ENOSPC;
		goto reject;
	}
	if (async && cmd->e2a_msg) {
		ret = -EINVAL;
		goto reject;
	}
	if (!cmd_mgr->users++)
		reinit_completion(&cmd_mgr->users_idle);
	cmd->tkn = cmd_mgr->next_tkn++;
	cmd->result = -EINPROGRESS;
	cmd->flags |= RWNX_CMD_FLAG_WAIT_PUSH;
	if (cmd->flags & RWNX_CMD_FLAG_REQ_CFM)
		cmd->flags |= RWNX_CMD_FLAG_WAIT_CFM;
	rwnx_cmd_get(cmd);
	list_add_tail(&cmd->list, &cmd_mgr->cmds);
	if (!cmd_mgr->queue_sz++ && cmd_mgr->hw)
		rwnx_wakeup_lock(cmd_mgr->hw->ws_tx);
	/* Schedule under the admission lock, before quiesce can close the queue. */
	WAKE_CMD_WORK(cmd_mgr);
	spin_unlock_bh(&cmd_mgr->lock);

	ret = 0;
	if (!async) {
		ret = wait_for_completion_killable(&cmd->complete);
		spin_lock_bh(&cmd_mgr->lock);
		if (ret && !(cmd->flags & RWNX_CMD_FLAG_DONE)) {
			if (cmd->flags & RWNX_CMD_FLAG_WAIT_PUSH)
				cmd_complete(cmd_mgr, cmd, ret);
			else
				/* Replies carry only reqid: do not admit a new generation. */
				cmd_mgr_abort(cmd_mgr, cmd, ret);
		}
		ret = cmd->result;
		spin_unlock_bh(&cmd_mgr->lock);
	}
	rwnx_cmd_free(cmd);
	cmd_mgr_user_put(cmd_mgr);
	return ret;

reject:
	spin_unlock_bh(&cmd_mgr->lock);
	rwnx_cmd_free(cmd);
	return ret;
}

int cmd_mgr_queue_force_defer(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
	if (cmd->e2a_msg) {
		rwnx_cmd_free(cmd);
		return -EINVAL;
	}
	cmd->flags |= RWNX_CMD_FLAG_NONBLOCK;
	return cmd_mgr_queue(cmd_mgr, cmd);
}

/* No runtime transport ACK callback is used; USB completion belongs to worker. */
static int cmd_mgr_llind(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
	return -EOPNOTSUPP;
}

void cmd_mgr_task_process(struct work_struct *work)
{
	struct rwnx_cmd_mgr *cmd_mgr = container_of(work, struct rwnx_cmd_mgr, cmdWork);
	struct rwnx_cmd *cmd;
	unsigned long done;
	int ret;

	for (;;) {
		spin_lock_bh(&cmd_mgr->lock);
		if (cmd_mgr->state != RWNX_CMD_MGR_STATE_INITED ||
		    list_empty(&cmd_mgr->cmds)) {
			spin_unlock_bh(&cmd_mgr->lock);
			break;
		}
		cmd = list_first_entry(&cmd_mgr->cmds, struct rwnx_cmd, list);
		rwnx_cmd_get(cmd);
		cmd_mgr->active = cmd;
		cmd->flags &= ~RWNX_CMD_FLAG_WAIT_PUSH;
		/* WAIT_ACK denotes transport completion, never a radio ACK. */
		cmd->flags |= RWNX_CMD_FLAG_WAIT_ACK;
		spin_unlock_bh(&cmd_mgr->lock);

#ifdef AICWF_SDIO_SUPPORT
		ret = aicwf_set_cmd_tx(container_of(cmd_mgr, struct aic_sdio_dev, cmd_mgr),
				       cmd->a2e_msg, RWNX_CMD_A2EMSG_LEN(cmd->a2e_msg));
#else
		ret = aicwf_set_cmd_tx(container_of(cmd_mgr, struct aic_usb_dev, cmd_mgr),
				       cmd->a2e_msg, RWNX_CMD_A2EMSG_LEN(cmd->a2e_msg));
#endif
		spin_lock_bh(&cmd_mgr->lock);
		if (!(cmd->flags & RWNX_CMD_FLAG_DONE)) {
			if (ret) {
				cmd_mgr_abort(cmd_mgr, cmd, ret);
			} else {
				cmd->flags &= ~RWNX_CMD_FLAG_WAIT_ACK;
				if (!(cmd->flags & RWNX_CMD_FLAG_REQ_CFM))
					cmd->result = 0;
				if (RWNX_CMD_WAIT_COMPLETE(cmd->flags))
					cmd_complete(cmd_mgr, cmd, cmd->result);
			}
		}
		spin_unlock_bh(&cmd_mgr->lock);

		done = wait_for_completion_timeout(&cmd->complete,
				msecs_to_jiffies(RWNX_80211_CMD_TIMEOUT_MS));
		spin_lock_bh(&cmd_mgr->lock);
		if (!done && !(cmd->flags & RWNX_CMD_FLAG_DONE))
			cmd_mgr_abort(cmd_mgr, cmd, -ETIMEDOUT);
		cmd_mgr->active = NULL;
		spin_unlock_bh(&cmd_mgr->lock);
		rwnx_cmd_free(cmd);
	}
}

static int cmd_mgr_run_callback(struct rwnx_hw *rwnx_hw, struct rwnx_cmd *cmd,
                                struct rwnx_cmd_e2amsg *msg, msg_cb_fct cb)
{
    int res;

    if (! cb){
        return 0;
    }
    //RWNX_DBG(RWNX_FN_ENTRY_STR);
#ifndef CONFIG_DEBUG_ATOMIC_SLEEP
	//spin_lock_bh(&rwnx_hw->cb_lock);
#endif
    res = cb(rwnx_hw, cmd, msg);
#ifndef CONFIG_DEBUG_ATOMIC_SLEEP
	//spin_unlock_bh(&rwnx_hw->cb_lock);
#endif

    return res;
}

/**
 *

 */
static int cmd_mgr_msgind(struct rwnx_cmd_mgr *cmd_mgr,
						  struct rwnx_cmd_e2amsg *msg, size_t received_len,
						  msg_cb_fct cb, size_t min_len)
{
#ifdef AICWF_SDIO_SUPPORT
	struct aic_sdio_dev *sdiodev = container_of(cmd_mgr, struct aic_sdio_dev, cmd_mgr);
	struct rwnx_hw *rwnx_hw = sdiodev->rwnx_hw;
#endif
#ifdef AICWF_USB_SUPPORT
	struct aic_usb_dev *usbdev = container_of(cmd_mgr, struct aic_usb_dev, cmd_mgr);
	struct rwnx_hw *rwnx_hw = usbdev->rwnx_hw;
#endif
	struct rwnx_cmd *cmd, *pos;
	bool found = false;
	int ret = 0;
	size_t header_len = offsetof(struct rwnx_cmd_e2amsg, param);

	if (!msg || received_len < header_len)
		return -EMSGSIZE;
	if (msg->param_len > RWNX_CMD_E2AMSG_LEN_MAX ||
		msg->param_len > received_len - header_len ||
		msg->param_len < min_len)
		ret = -EMSGSIZE;

	spin_lock_bh(&cmd_mgr->lock);
	if (cmd_mgr->state != RWNX_CMD_MGR_STATE_INITED) {
		spin_unlock_bh(&cmd_mgr->lock);
		return -ESHUTDOWN;
	}
	list_for_each_entry_safe(cmd, pos, &cmd_mgr->cmds, list) {
		if (cmd != cmd_mgr->active || cmd->reqid != msg->id ||
		    (cmd->flags & RWNX_CMD_FLAG_DONE) ||
		    !(cmd->flags & RWNX_CMD_FLAG_WAIT_CFM))
			continue;

		if (cmd->e2a_msg &&
			(msg->param_len > cmd->e2a_msg_len ||
			 msg->param_len < cmd->e2a_msg_min_len))
			ret = -EMSGSIZE;

		/* Never call a handler or copy a malformed confirmation. */
		if (!ret) {
			int cb_ret = cmd_mgr_run_callback(rwnx_hw, cmd, msg, cb);

			if (cb_ret > 0)
				continue;
			if (cb_ret < 0)
				ret = cb_ret;
		}

		found = true;
		cmd->flags &= ~RWNX_CMD_FLAG_WAIT_CFM;
		cmd->result = ret;
		if (!ret && cmd->e2a_msg && msg->param_len)
			memcpy(cmd->e2a_msg, msg->param, msg->param_len);
		if (RWNX_CMD_WAIT_COMPLETE(cmd->flags))
			cmd_complete(cmd_mgr, cmd, ret);
		break;
	}
	spin_unlock_bh(&cmd_mgr->lock);

	if (!found && !ret)
		ret = cmd_mgr_run_callback(rwnx_hw, NULL, msg, cb);

	return ret;
}


static void cmd_mgr_print(struct rwnx_cmd_mgr *cmd_mgr)
{
	spin_lock_bh(&cmd_mgr->lock);
	RWNX_DBG("q_sz/max: %u / %u - next tkn: %u\n",
		 cmd_mgr->queue_sz, cmd_mgr->max_queue_sz, cmd_mgr->next_tkn);
	spin_unlock_bh(&cmd_mgr->lock);
}

static void cmd_mgr_drain(struct rwnx_cmd_mgr *cmd_mgr)
{
	spin_lock_bh(&cmd_mgr->lock);
	cmd_mgr_abort(cmd_mgr, NULL, -ESHUTDOWN);
	spin_unlock_bh(&cmd_mgr->lock);
}

void rwnx_cmd_mgr_init(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_hw *hw)
{
	INIT_LIST_HEAD(&cmd_mgr->cmds);
	spin_lock_init(&cmd_mgr->lock);
	init_completion(&cmd_mgr->users_idle);
	complete_all(&cmd_mgr->users_idle);
	cmd_mgr->hw = hw;
	cmd_mgr->active = NULL;
	cmd_mgr->users = 0;
	cmd_mgr->queue_sz = 0;
	cmd_mgr->next_tkn = 0;
	cmd_mgr->state = RWNX_CMD_MGR_STATE_INITED;
	cmd_mgr->max_queue_sz = RWNX_CMD_MAX_QUEUED;
	cmd_mgr->queue = cmd_mgr_queue;
	cmd_mgr->print = cmd_mgr_print;
	cmd_mgr->drain = cmd_mgr_drain;
	cmd_mgr->llind = cmd_mgr_llind;
	cmd_mgr->msgind = cmd_mgr_msgind;
	INIT_WORK(&cmd_mgr->cmdWork, cmd_mgr_task_process);
	cmd_mgr->cmd_wq = create_singlethread_workqueue("cmd_wq");
	if (!cmd_mgr->cmd_wq)
		cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
}

/* Producers/RX stop first; wake and join owners before releasing hw or work. */
void rwnx_cmd_mgr_quiesce(struct rwnx_cmd_mgr *cmd_mgr)
{
	spin_lock_bh(&cmd_mgr->lock);
	cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
	while (!list_empty(&cmd_mgr->cmds)) {
		struct rwnx_cmd *cmd;

		cmd = list_first_entry(&cmd_mgr->cmds, struct rwnx_cmd, list);
		cmd_complete(cmd_mgr, cmd, -ESHUTDOWN);
	}
	spin_unlock_bh(&cmd_mgr->lock);
	if (cmd_mgr->cmd_wq)
		cancel_work_sync(&cmd_mgr->cmdWork);
	wait_for_completion(&cmd_mgr->users_idle);
	/* Pairs with the final caller unlock after users_idle completion. */
	spin_lock_bh(&cmd_mgr->lock);
	cmd_mgr->hw = NULL;
	spin_unlock_bh(&cmd_mgr->lock);
}

void rwnx_cmd_mgr_deinit(struct rwnx_cmd_mgr *cmd_mgr)
{
	if (!cmd_mgr->print || !cmd_mgr->drain)
		return;
	rwnx_cmd_mgr_quiesce(cmd_mgr);
	if (cmd_mgr->cmd_wq) {
		destroy_workqueue(cmd_mgr->cmd_wq);
		cmd_mgr->cmd_wq = NULL;
	}
	cmd_mgr->state = RWNX_CMD_MGR_STATE_DEINIT;
}

/* The worker owns TX; recovery may send only after the manager is quiesced. */
int aicwf_set_cmd_tx(void *dev, struct lmac_msg *msg, uint len)
{
	struct aicwf_bus *bus;
	u8 *buffer;
	u16 index = 8;

	if (!dev || !msg)
		return -EINVAL;
#ifdef AICWF_SDIO_SUPPORT
	bus = ((struct aic_sdio_dev *)dev)->bus_if;
#else
	if (((struct aic_usb_dev *)dev)->state != USB_UP_ST)
		return -ENETDOWN;
	bus = ((struct aic_usb_dev *)dev)->bus_if;
#endif
	if (!bus || !bus->cmd_buf)
		return -ENODEV;
	if (bus->state == BUS_DOWN_ST)
		return -ENETDOWN;
	if (len != sizeof(*msg) + msg->param_len || len > CMD_BUF_MAX - 8)
		return -EMSGSIZE;
	buffer = bus->cmd_buf;
	memset(buffer, 0, CMD_BUF_MAX);
	buffer[0] = (len + 4) & 0xff;
	buffer[1] = ((len + 4) >> 8) & 0x0f;
	buffer[2] = 0x11;
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

#ifdef AICWF_USB_SUPPORT
/* The recovery worker has drained normal commands; never reopen their queue. */
int aicwf_usb_send_reboot(struct aic_usb_dev *usb)
{
	struct dbg_start_app_req *req;
	struct lmac_msg *msg;
	size_t len = sizeof(*msg) + sizeof(*req);
	int ret;

	if (!usb)
		return -EINVAL;
	msg = kzalloc(len, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	msg->id = DBG_START_APP_REQ;
	msg->dest_id = TASK_DBG;
	msg->src_id = DRV_TASK_ID;
	msg->param_len = sizeof(*req);
	req = (void *)msg->param;
	req->bootaddr = 2000;
	req->boottype = HOST_START_APP_REBOOT;
	/* A successful USB transfer is not proof of a firmware reboot. */
	ret = aicwf_set_cmd_tx(usb, msg, len);
	kfree(msg);
	return ret;
}
#endif
