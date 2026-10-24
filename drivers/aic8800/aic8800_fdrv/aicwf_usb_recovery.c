// SPDX-License-Identifier: GPL-2.0
/* Full firmware restart after an uncertain command; never revive old replies. */
#include <linux/jiffies.h>
#include <linux/ratelimit.h>
#include <linux/workqueue.h>
#include "aicwf_usb.h"

#define AICWF_RECOVERY_DISCONNECT_MS 5000

/* Survives USB re-enumeration, bounding repeated failures across fresh probes. */
static DEFINE_RATELIMIT_STATE(aicwf_recovery_limit, 60 * HZ, 1);

static void aicwf_usb_recovery_timeout(struct work_struct *work)
{
	struct aic_usb_dev *usb = container_of(to_delayed_work(work),
					     struct aic_usb_dev, recovery_timeout_work);
	bool failed = false;

	spin_lock_bh(&usb->recovery_lock);
	if (!usb->recovery_stopping &&
	    usb->recovery_state == AICWF_RECOVERY_WAIT_DISCONNECT) {
		usb->recovery_state = AICWF_RECOVERY_FAILED;
		failed = true;
	}
	spin_unlock_bh(&usb->recovery_lock);
	if (failed)
		dev_err(usb->dev, "firmware reboot did not disconnect; power-cycle required\n");
}

static void aicwf_usb_recovery_work(struct work_struct *work)
{
	struct aic_usb_dev *usb = container_of(work, struct aic_usb_dev, recovery_work);
	int ret, reason;

	spin_lock_bh(&usb->recovery_lock);
	if (usb->recovery_stopping) {
		spin_unlock_bh(&usb->recovery_lock);
		return;
	}
	usb->recovery_state = AICWF_RECOVERY_RUNNING;
	reason = usb->recovery_reason;
	spin_unlock_bh(&usb->recovery_lock);
	dev_warn(usb->dev, "command failure %d; restarting firmware for recovery\n", reason);

	/* Joins the normal sender and callers before borrowing its buffer and URB. */
	rwnx_cmd_mgr_quiesce(&usb->cmd_mgr);
	spin_lock_bh(&usb->recovery_lock);
	if (usb->recovery_stopping) {
		spin_unlock_bh(&usb->recovery_lock);
		return;
	}
	spin_unlock_bh(&usb->recovery_lock);

	/* Fixed reboot request only; manager remains CRASHED until actual removal. */
	ret = aicwf_usb_send_reboot(usb);
	spin_lock_bh(&usb->recovery_lock);
	if (usb->recovery_stopping) {
		spin_unlock_bh(&usb->recovery_lock);
		return;
	}
	if (ret) {
		usb->recovery_state = AICWF_RECOVERY_FAILED;
	} else {
		usb->recovery_state = AICWF_RECOVERY_WAIT_DISCONNECT;
		queue_delayed_work(system_long_wq, &usb->recovery_timeout_work,
				   msecs_to_jiffies(AICWF_RECOVERY_DISCONNECT_MS));
	}
	spin_unlock_bh(&usb->recovery_lock);
	if (ret)
		dev_err(usb->dev, "firmware reboot failed: %d; power-cycle required\n", ret);
}

void aicwf_usb_recovery_init(struct aic_usb_dev *usb)
{
	spin_lock_init(&usb->recovery_lock);
	INIT_WORK(&usb->recovery_work, aicwf_usb_recovery_work);
	INIT_DELAYED_WORK(&usb->recovery_timeout_work, aicwf_usb_recovery_timeout);
	usb->recovery_state = AICWF_RECOVERY_IDLE;
	usb->recovery_stopping = false;
	usb->recovery_armed = false;
	usb->recovery_reason = 0;
}

/* Init failures never start reboot loops; arm only after a complete probe. */
void aicwf_usb_recovery_start(struct aic_usb_dev *usb)
{
	spin_lock_bh(&usb->recovery_lock);
	if (!usb->recovery_stopping && usb->recovery_state == AICWF_RECOVERY_IDLE)
		usb->recovery_armed = true;
	spin_unlock_bh(&usb->recovery_lock);
}

/* Can run under the command lock. No sleeping or driver teardown here. */
void aicwf_usb_recovery_request(struct aic_usb_dev *usb, int reason)
{
	bool limited = false;

	spin_lock_bh(&usb->recovery_lock);
	if (!usb->recovery_armed || usb->recovery_stopping ||
	    usb->recovery_state != AICWF_RECOVERY_IDLE) {
		spin_unlock_bh(&usb->recovery_lock);
		return;
	}
	usb->recovery_reason = reason;
	if (!__ratelimit(&aicwf_recovery_limit)) {
		usb->recovery_state = AICWF_RECOVERY_FAILED;
		limited = true;
	} else {
		usb->recovery_state = AICWF_RECOVERY_QUEUED;
		queue_work(system_long_wq, &usb->recovery_work);
	}
	spin_unlock_bh(&usb->recovery_lock);
	if (limited)
		dev_warn(usb->dev, "automatic firmware reboot rate-limited; power-cycle required\n");
}

/* Disconnect calls this before taking teardown locks or releasing USB state. */
void aicwf_usb_recovery_stop(struct aic_usb_dev *usb)
{
	spin_lock_bh(&usb->recovery_lock);
	usb->recovery_stopping = true;
	usb->recovery_armed = false;
	usb->recovery_state = AICWF_RECOVERY_STOPPED;
	spin_unlock_bh(&usb->recovery_lock);
	cancel_work_sync(&usb->recovery_work);
	cancel_delayed_work_sync(&usb->recovery_timeout_work);
}
