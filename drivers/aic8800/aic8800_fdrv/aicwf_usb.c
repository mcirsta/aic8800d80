/**
 * aicwf_usb.c
 *
 * USB function declarations
 *
 * Copyright (C) AICSemi 2018-2020
 */

#include <linux/usb.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include "aicwf_txrxif.h"
#include "aicwf_usb.h"
#include "rwnx_tx.h"
#include "rwnx_defs.h"
#include "rwnx_state.h"
#include "usb_host.h"
#include "rwnx_platform.h"
#include "rwnx_msg_tx.h"

#ifdef CONFIG_GPIO_WAKEUP
#ifdef CONFIG_PLATFORM_ROCKCHIP
#include <linux/rfkill-wlan.h>
#endif
static int wakeup_enable;
static u32 hostwake_irq_num;
atomic_t irq_count;
spinlock_t irq_lock;
#endif

#include <linux/semaphore.h>
extern struct semaphore aicwf_deinit_sem;
extern atomic_t aicwf_deinit_atomic;
#define SEM_TIMOUT 2000


#ifdef CONFIG_TXRX_THREAD_PRIO

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0))
#include "uapi/linux/sched/types.h"
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
#include "linux/sched/types.h"
#else
#include "linux/sched/rt.h"
#endif

int bustx_thread_prio = 1;
module_param(bustx_thread_prio, int, 0);
int busrx_thread_prio = 1;
module_param(busrx_thread_prio, int, 0);
#endif

#ifdef CONFIG_USB_RX_AGGR
bool aicwf_usb_rx_aggr = true;
#else
bool aicwf_usb_rx_aggr = false;
#endif
atomic_t rx_urb_cnt;
bool rx_urb_sched = false;
u32 aicwf_usb_max_pkt_size = AICWF_USB_MAX_PKT_SIZE;

void aicwf_usb_tx_flowctrl(struct rwnx_hw *rwnx_hw, bool state)
{
    struct rwnx_vif *rwnx_vif;
	int idx = srcu_read_lock(&rwnx_hw->vif_readers);

    list_for_each_entry_rcu(rwnx_vif, &rwnx_hw->vifs, list,
			   srcu_read_lock_held(&rwnx_hw->vif_readers)) {
	if (!rwnx_vif->ndev || !smp_load_acquire(&rwnx_vif->up) ||
	    !netif_running(rwnx_vif->ndev))
            continue;
        if (state)
            netif_tx_stop_all_queues(rwnx_vif->ndev);//netif_stop_queue(rwnx_vif->ndev);
        else
            netif_tx_wake_all_queues(rwnx_vif->ndev);//netif_wake_queue(rwnx_vif->ndev);
	}
	srcu_read_unlock(&rwnx_hw->vif_readers, idx);
}

static struct aicwf_usb_buf *aicwf_usb_tx_dequeue(struct aic_usb_dev *usb_dev,
    struct list_head *q, int *counter, spinlock_t *qlock)
{
    unsigned long flags;
    struct aicwf_usb_buf *usb_buf;

    spin_lock_irqsave(qlock, flags);
    if (list_empty(q)) {
        usb_buf = NULL;
    } else {
        usb_buf = list_first_entry(q, struct aicwf_usb_buf, list);
        list_del_init(&usb_buf->list);
        if (counter)
            (*counter)--;
    }
    spin_unlock_irqrestore(qlock, flags);
    return usb_buf;
}

static void aicwf_usb_tx_queue(struct aic_usb_dev *usb_dev,
    struct list_head *q, struct aicwf_usb_buf *usb_buf, int *counter,
    spinlock_t *qlock)
{
    unsigned long flags;

    spin_lock_irqsave(qlock, flags);
    list_add_tail(&usb_buf->list, q);
    (*counter)++;
    spin_unlock_irqrestore(qlock, flags);
}

static struct aicwf_usb_buf *aicwf_usb_rx_buf_get(struct aic_usb_dev *usb_dev)
{
    unsigned long flags;
    struct aicwf_usb_buf *usb_buf;

    spin_lock_irqsave(&usb_dev->rx_free_lock, flags);
    if (list_empty(&usb_dev->rx_free_list)) {
        usb_buf = NULL;
    } else {
        usb_buf = list_first_entry(&usb_dev->rx_free_list, struct aicwf_usb_buf, list);
        list_del_init(&usb_buf->list);
    }
    spin_unlock_irqrestore(&usb_dev->rx_free_lock, flags);
    return usb_buf;
}

static void aicwf_usb_rx_buf_put(struct aic_usb_dev *usb_dev, struct aicwf_usb_buf *usb_buf)
{
    unsigned long flags;

    spin_lock_irqsave(&usb_dev->rx_free_lock, flags);
    list_add_tail(&usb_buf->list, &usb_dev->rx_free_list);
    spin_unlock_irqrestore(&usb_dev->rx_free_lock, flags);
}

#ifdef CONFIG_USB_MSG_IN_EP
static struct aicwf_usb_buf *aicwf_usb_msg_rx_buf_get(struct aic_usb_dev *usb_dev)
{
    unsigned long flags;
    struct aicwf_usb_buf *usb_buf;

    spin_lock_irqsave(&usb_dev->msg_rx_free_lock, flags);
    if (list_empty(&usb_dev->msg_rx_free_list)) {
        usb_buf = NULL;
    } else {
        usb_buf = list_first_entry(&usb_dev->msg_rx_free_list, struct aicwf_usb_buf, list);
        list_del_init(&usb_buf->list);
    }
    spin_unlock_irqrestore(&usb_dev->msg_rx_free_lock, flags);
    return usb_buf;
}

static void aicwf_usb_msg_rx_buf_put(struct aic_usb_dev *usb_dev, struct aicwf_usb_buf *usb_buf)
{
    unsigned long flags;

    spin_lock_irqsave(&usb_dev->msg_rx_free_lock, flags);
    list_add_tail(&usb_buf->list, &usb_dev->msg_rx_free_list);
    spin_unlock_irqrestore(&usb_dev->msg_rx_free_lock, flags);
}
#endif

void rwnx_stop_sta_all_queues(void *sta, struct rwnx_hw *rwnx_hw)
{
    u8 tid;
    struct rwnx_txq *txq;
	struct rwnx_sta *sta_tmp = (struct rwnx_sta *)sta;
    for(tid=0; tid<8; tid++) {
        txq = rwnx_txq_sta_get(sta_tmp, tid, rwnx_hw);
        netif_stop_subqueue(txq->ndev, txq->ndev_idx);
    }
}

void rwnx_wake_sta_all_queues(void *sta, struct rwnx_hw *rwnx_hw)
{
    u8 tid;
    struct rwnx_txq *txq;
	struct rwnx_sta *sta_tmp = (struct rwnx_sta *)sta;
    for(tid=0; tid<8; tid++) {
        txq = rwnx_txq_sta_get(sta_tmp, tid, rwnx_hw);
        netif_wake_subqueue(txq->ndev, txq->ndev_idx);
    }
}

static void usb_txc_sta_flowctrl(struct aicwf_usb_buf *usb_buf, struct aic_usb_dev *usb_dev)
{
#ifdef CONFIG_PER_STA_FC
    unsigned long flags;
	struct rwnx_sta *sta;
	struct txdesc_api *hostdesc;
	u8 sta_idx;
	int srcu_idx;
	hostdesc = (struct txdesc_api *)((u8 *)usb_buf->urb->transfer_buffer + 4);
	//printk("txcpl: sta %d\n", hostdesc->host.staid);
	sta_idx = hostdesc->host.staid;
	if(sta_idx < NX_REMOTE_STA_MAX && !(hostdesc->host.flags & TXU_CNTRL_MGMT)) {
		struct rwnx_vif *vif = NULL;
		sta = &usb_dev->rwnx_hw->sta_table[sta_idx];
		srcu_idx = srcu_read_lock(&usb_dev->rwnx_hw->vif_readers);
		vif = rwnx_vif_lookup(usb_dev->rwnx_hw, READ_ONCE(sta->vif_idx));
		spin_lock_irqsave(&usb_dev->tx_flow_lock, flags);
		atomic_dec(&usb_dev->rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt);
		//printk("sta:%d, pending:%d, flowctrl=%d\n", sta->sta_idx, sta->tx_pending_cnt, sta->flowctrl);
		if (vif && rwnx_sta_is_valid(sta) &&
		    RWNX_VIF_TYPE(vif) == NL80211_IFTYPE_AP) {
			if(atomic_read(&usb_dev->rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt) < AICWF_USB_FC_PERSTA_LOW_WATER &&
							usb_dev->rwnx_hw->sta_flowctrl[sta_idx].flowctrl) {
				//AICWFDBG(LOGDEBUG, "sta 0x%x:0x%x, %d pending %d, wake\n", sta->mac_addr[4], sta->mac_addr[5], sta->sta_idx, atomic_read(&usb_dev->rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt));
				if(!usb_dev->tbusy)
					rwnx_wake_sta_all_queues((void *)sta, usb_dev->rwnx_hw);
				usb_dev->rwnx_hw->sta_flowctrl[sta_idx].flowctrl = 0;
			}
		}
		spin_unlock_irqrestore(&usb_dev->tx_flow_lock, flags);
		srcu_read_unlock(&usb_dev->rwnx_hw->vif_readers, srcu_idx);
	}
#endif
}

/* Called only after USB has relinquished the transfer buffer. */
static void aicwf_usb_tx_release(struct aicwf_usb_buf *usb_buf)
{
	struct aic_usb_dev *usb_dev = usb_buf->usbdev;
	unsigned long flags;

	usb_txc_sta_flowctrl(usb_buf, usb_dev);
	kfree(usb_buf->usb_align_data);
	usb_buf->usb_align_data = NULL;
#ifndef CONFIG_USB_TX_AGGR
	dev_kfree_skb_any(usb_buf->skb);
	kfree(usb_buf->tx_data);
	usb_buf->skb = NULL;
	usb_buf->tx_data = NULL;
	usb_buf->cfm = false;
#else
	usb_buf->aggr_cnt = 0;
#endif
	aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_free_list, usb_buf,
			   &usb_dev->tx_free_count, &usb_dev->tx_free_lock);
	spin_lock_irqsave(&usb_dev->tx_flow_lock, flags);
	if (!READ_ONCE(usb_dev->tx_stopping) &&
	    usb_dev->tx_free_count > AICWF_USB_TX_HIGH_WATER && usb_dev->tbusy) {
		usb_dev->tbusy = false;
		aicwf_usb_tx_flowctrl(usb_dev->rwnx_hw, false);
	}
	spin_unlock_irqrestore(&usb_dev->tx_flow_lock, flags);
}

#ifndef CONFIG_USB_TX_AGGR
static void aicwf_usb_tx_failed_work(struct work_struct *work)
{
	struct aic_usb_dev *usb_dev = container_of(work, struct aic_usb_dev,
						 tx_failed_work);
	struct rwnx_hw *hw = usb_dev->rwnx_hw;
	struct aicwf_usb_buf *usb_buf;
	struct sk_buff *skb;

	while ((usb_buf = aicwf_usb_tx_dequeue(usb_dev, &usb_dev->tx_failed_list,
					      NULL, &usb_dev->tx_failed_lock))) {
		spin_lock_bh(&hw->tx_lock);
		skb = aicwf_usb_host_txdesc_take(&hw->usb_env, usb_buf->cfm_tag);
		if (skb)
			rwnx_usb_tx_drop(hw, skb);
		spin_unlock_bh(&hw->tx_lock);
		aicwf_usb_tx_release(usb_buf);
	}
}
#endif

static void aicwf_usb_tx_finish(struct aicwf_usb_buf *usb_buf, int status)
{
#ifndef CONFIG_USB_TX_AGGR
	if (status && usb_buf->cfm) {
		struct aic_usb_dev *usb_dev = usb_buf->usbdev;
		unsigned long flags;

		/* tx_lock is also taken with BHs disabled; never take it in an IRQ. */
		spin_lock_irqsave(&usb_dev->tx_failed_lock, flags);
		list_add_tail(&usb_buf->list, &usb_dev->tx_failed_list);
		spin_unlock_irqrestore(&usb_dev->tx_failed_lock, flags);
		schedule_work(&usb_dev->tx_failed_work);
		return;
	}
#endif
	aicwf_usb_tx_release(usb_buf);
}

static void aicwf_usb_tx_complete(struct urb *urb)
{
	aicwf_usb_tx_finish(urb->context, urb->status);
}

void aicwf_usb_rx_submit_all_urb_(struct aic_usb_dev *usb_dev);

#ifdef CONFIG_PREALLOC_RX_SKB
static void aicwf_usb_rx_complete(struct urb *urb)
{
    struct aicwf_usb_buf *usb_buf = (struct aicwf_usb_buf *) urb->context;
    struct aic_usb_dev *usb_dev = usb_buf->usbdev;
    struct aicwf_rx_priv* rx_priv = usb_dev->rx_priv;
    struct rx_buff *rx_buff = NULL;
    unsigned long flags = 0;

    rx_buff = usb_buf->rx_buff;
    usb_buf->rx_buff = NULL;

    atomic_dec(&rx_urb_cnt);
    if(atomic_read(&rx_urb_cnt) < 10){
        AICWFDBG(LOGDEBUG, "%s %d \r\n", __func__, atomic_read(&rx_urb_cnt));
        //printk("%s %d \r\n", __func__, atomic_read(&rx_urb_cnt));
    }

	/* Pairs with the initialized owner publication in cfg80211 init. */
	if (!smp_load_acquire(&usb_dev->rwnx_hw)) {
		aicwf_prealloc_rxbuff_free(rx_buff, &rx_priv->rxbuff_lock);
		aicwf_usb_rx_buf_put(usb_dev, usb_buf);
		/* Keep reply buffers available until initialization publishes hw. */
		aicwf_usb_rx_submit_all_urb_(usb_dev);
		return;
	}

    if (urb->actual_length > urb->transfer_buffer_length) {
        aicwf_prealloc_rxbuff_free(rx_buff, &rx_priv->rxbuff_lock);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        aicwf_usb_rx_submit_all_urb_(usb_dev);
        return;
    }

    if (urb->status != 0 || !urb->actual_length) {
        aicwf_prealloc_rxbuff_free(rx_buff, &rx_priv->rxbuff_lock);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        if(urb->status < 0){
            AICWFDBG(LOGDEBUG, "%s urb->status:%d \r\n", __func__, urb->status);
            return;
        }else{
            //schedule_work(&usb_dev->rx_urb_work);
            aicwf_usb_rx_submit_all_urb_(usb_dev);
            return;
        }
    }

    if (usb_dev->state == USB_UP_ST) {
        spin_lock_irqsave(&rx_priv->rxqlock, flags);
        //if (aicwf_usb_rx_aggr) {
            rx_buff->len = urb->actual_length;
            //printk("%s rx_buff->len:%d \r\n", __func__, rx_buff->len);
        //}

        if(!aicwf_rxbuff_enqueue(usb_dev->dev, &rx_priv->rxq, rx_buff)){
            spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
            usb_err("rx_priv->rxq is over flow!!!\n");
            aicwf_prealloc_rxbuff_free(rx_buff, &rx_priv->rxbuff_lock);
            aicwf_usb_rx_buf_put(usb_dev, usb_buf);
            aicwf_usb_rx_submit_all_urb_(usb_dev);
            return;
        }
        spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
        atomic_inc(&rx_priv->rx_cnt);

        if(atomic_read(&rx_priv->rx_cnt) == 1){
            complete(&rx_priv->usbdev->bus_if->busrx_trgg);
        }

        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        aicwf_usb_rx_submit_all_urb_(usb_dev);
    } else {
        aicwf_prealloc_rxbuff_free(rx_buff, &rx_priv->rxbuff_lock);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
    }
}

#else
static void aicwf_usb_rx_complete(struct urb *urb)
{
    struct aicwf_usb_buf *usb_buf = (struct aicwf_usb_buf *) urb->context;
    struct aic_usb_dev *usb_dev = usb_buf->usbdev;
    struct aicwf_rx_priv* rx_priv = usb_dev->rx_priv;
    struct sk_buff *skb = NULL;
    unsigned long flags = 0;

    skb = usb_buf->skb;
    usb_buf->skb = NULL;

	atomic_dec(&rx_urb_cnt);
	if(atomic_read(&rx_urb_cnt) < 10){
		AICWFDBG(LOGDEBUG, "%s %d \r\n", __func__, atomic_read(&rx_urb_cnt));
		//printk("%s %d \r\n", __func__, atomic_read(&rx_urb_cnt));
	}

	/* Pairs with the initialized owner publication in cfg80211 init. */
	if (!smp_load_acquire(&usb_dev->rwnx_hw)) {
		aicwf_dev_skb_free(skb);
		aicwf_usb_rx_buf_put(usb_dev, usb_buf);
		/* The refill helper refuses submissions once teardown sets DOWN. */
		aicwf_usb_rx_submit_all_urb_(usb_dev);
		return;
	}

    if (urb->actual_length > urb->transfer_buffer_length) {
        usb_err("urb_rx len error %u/%u\n", urb->actual_length, urb->transfer_buffer_length);
        aicwf_dev_skb_free(skb);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        aicwf_usb_rx_submit_all_urb_(usb_dev);
        return;
    }

    if (urb->status != 0 || !urb->actual_length) {
        aicwf_dev_skb_free(skb);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
		if(urb->status < 0){
			AICWFDBG(LOGDEBUG, "%s urb->status:%d \r\n", __func__, urb->status);
			return;
		}else{
			//schedule_work(&usb_dev->rx_urb_work);
			aicwf_usb_rx_submit_all_urb_(usb_dev);
        	return;
		}
    }

#if 0
    if ((urb->actual_length > 1600 * 30) && (aicwf_usb_rx_aggr)) {
	printk("r%d\n", urb->actual_length);
    }
#endif

    if (usb_dev->state == USB_UP_ST) {

        skb_put(skb, urb->actual_length);

        if (aicwf_usb_rx_aggr) {
            skb->len = urb->actual_length;
        } else {
#ifdef CONFIG_USB_RX_REASSEMBLE
            bool pkt_check = false;
            if (rx_priv->rx_reassemble_skb) {
                u32 frag_len = skb->len;
                struct sk_buff *reassemble_skb = rx_priv->rx_reassemble_skb;
                bool reassemble_valid = false;
                bool reassemble_done = false;
                if ((rx_priv->rx_reassemble_cur_frags + 1) == rx_priv->rx_reassemble_total_frags) {
                    if ((rx_priv->rx_reassemble_cur_len + frag_len) == rx_priv->rx_reassemble_total_len) {
                        reassemble_valid = true;
                        reassemble_done = true;
                    }
                } else {
                    if (frag_len == aicwf_usb_max_pkt_size) {
                        reassemble_valid = true;
                    }
                }

                if (reassemble_valid) {
                    memcpy((reassemble_skb->data + reassemble_skb->len), skb->data, frag_len);
                    skb_put(reassemble_skb, skb->len);
                    rx_priv->rx_reassemble_cur_len += frag_len;
                    rx_priv->rx_reassemble_cur_frags++;
                    aicwf_dev_skb_free(skb);
                    if (reassemble_done) {
                        skb = reassemble_skb;
                        rx_priv->rx_reassemble_skb = NULL;
                        rx_priv->rx_reassemble_total_len = 0;
                        rx_priv->rx_reassemble_cur_len = 0;
                        rx_priv->rx_reassemble_total_frags = 0;
                        rx_priv->rx_reassemble_cur_frags = 0;
                    } else {
                        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
                        aicwf_usb_rx_submit_all_urb_(usb_dev);
                        return;
                    }
                } else {
                    usb_err("invalid frag pkt, len=%u/%u/%u, frags=%u/%u\n", frag_len,
                        rx_priv->rx_reassemble_cur_len, rx_priv->rx_reassemble_cur_len,
                        rx_priv->rx_reassemble_cur_frags, rx_priv->rx_reassemble_total_frags);
                    aicwf_dev_skb_free(reassemble_skb);
                    rx_priv->rx_reassemble_skb = NULL;
                    rx_priv->rx_reassemble_total_len = 0;
                    rx_priv->rx_reassemble_cur_len = 0;
                    rx_priv->rx_reassemble_total_frags = 0;
                    rx_priv->rx_reassemble_cur_frags = 0;
                    pkt_check = true;
                }
            } else {
                pkt_check = true;
            }

            if (pkt_check) {
                bool pkt_drop = false;
                u8 type = skb->data[2];
                u32 pkt_len = skb->data[0] | (skb->data[1] << 8);
                if ((type & USB_TYPE_CFG) != USB_TYPE_CFG) {
                    u32 pkt_total_len = pkt_len + RX_HWHRD_LEN;
                    if ((pkt_total_len > aicwf_usb_max_pkt_size) && (skb->len == aicwf_usb_max_pkt_size)) {
                        AICWFDBG(LOGINFO, "reassemble pkt, len=%u\n", pkt_total_len);
                        struct sk_buff *reassemble_skb = __dev_alloc_skb(pkt_total_len, GFP_ATOMIC/*GFP_KERNEL*/);
                        if (reassemble_skb) {
                            memcpy(reassemble_skb->data, skb->data, skb->len);
                            skb_put(reassemble_skb, skb->len);
                            rx_priv->rx_reassemble_skb = reassemble_skb;
                            rx_priv->rx_reassemble_total_len = pkt_total_len;
                            rx_priv->rx_reassemble_cur_len = skb->len;
                            rx_priv->rx_reassemble_total_frags = ALIGN(pkt_total_len, aicwf_usb_max_pkt_size) / aicwf_usb_max_pkt_size;
                            rx_priv->rx_reassemble_cur_frags = 1;
                        } else {
                            usb_err("reassemble pkt alloc fail, len=%u\n", pkt_total_len);
                        }
                        aicwf_dev_skb_free(skb);
                        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
                        aicwf_usb_rx_submit_all_urb_(usb_dev);
                        return;
                    } else if (pkt_total_len != skb->len) {
                        usb_err("invalid DATA, len=%u/%u\n", pkt_len, skb->len);
                        pkt_drop = true;
                    }
                } else {
                    if (type == USB_TYPE_CFG_CMD_RSP) {
                        u32 pkt_total_len = ALIGN((pkt_len + 4), 4);
                        if ((pkt_total_len > aicwf_usb_max_pkt_size) && (skb->len == aicwf_usb_max_pkt_size)) {
                            AICWFDBG(LOGINFO, "reassemble pkt, len=%u\n", pkt_total_len);
                            struct sk_buff *reassemble_skb = __dev_alloc_skb(pkt_total_len, GFP_ATOMIC/*GFP_KERNEL*/);
                            if (reassemble_skb) {
                                memcpy(reassemble_skb->data, skb->data, skb->len);
                                skb_put(reassemble_skb, skb->len);
                                rx_priv->rx_reassemble_skb = reassemble_skb;
                                rx_priv->rx_reassemble_total_len = pkt_total_len;
                                rx_priv->rx_reassemble_cur_len = skb->len;
                                rx_priv->rx_reassemble_total_frags = ALIGN(pkt_total_len, aicwf_usb_max_pkt_size) / aicwf_usb_max_pkt_size;
                                rx_priv->rx_reassemble_cur_frags = 1;
                            } else {
                                usb_err("reassemble pkt alloc fail, len=%u\n", pkt_total_len);
                            }
                            aicwf_dev_skb_free(skb);
                            aicwf_usb_rx_buf_put(usb_dev, usb_buf);
                            aicwf_usb_rx_submit_all_urb_(usb_dev);
                            return;
                        } else if (pkt_total_len != skb->len) {
                            usb_err("invalid CMD_RSP, len=%u/%u\n", pkt_len, skb->len);
                            pkt_drop = true;
                        }
                    } else if (type == USB_TYPE_CFG_DATA_CFM) {
                        if (!((pkt_len == 8) && (skb->len == 12))) {
                            usb_err("invalid DATA_CFM, len=%u/%u\n", pkt_len, skb->len);
                            pkt_drop = true;
                        }
                    } else {
                        usb_err("invalid pkt, type=0x%x, len=%u/%u\n", type, pkt_len, skb->len);
                        pkt_drop = true;
                    }
                }

                if (pkt_drop) {
                    aicwf_dev_skb_free(skb);
                    aicwf_usb_rx_buf_put(usb_dev, usb_buf);
                    aicwf_usb_rx_submit_all_urb_(usb_dev);
                    return;
                }
            }
#endif
        }

        spin_lock_irqsave(&rx_priv->rxqlock, flags);
        if(!aicwf_rxframe_enqueue(usb_dev->dev, &rx_priv->rxq, skb)){
            spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
            usb_err("rx_priv->rxq is over flow!!!\n");
            aicwf_dev_skb_free(skb);
            aicwf_usb_rx_buf_put(usb_dev, usb_buf);
            aicwf_usb_rx_submit_all_urb_(usb_dev);
            return;
        }
        spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
        atomic_inc(&rx_priv->rx_cnt);
		
#ifndef CONFIG_RX_TASKLET 
		//if(!rx_priv->rx_thread_working && (atomic_read(&rx_priv->rx_cnt)>0)){
		if(atomic_read(&rx_priv->rx_cnt) == 1){
        	complete(&rx_priv->usbdev->bus_if->busrx_trgg);
		}
#else
        tasklet_schedule(&rx_priv->usbdev->recv_tasklet);
#endif
		
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        aicwf_usb_rx_submit_all_urb_(usb_dev);
        //schedule_work(&usb_dev->rx_urb_work);
    } else {
        aicwf_dev_skb_free(skb);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
    }
}
#endif

#ifdef CONFIG_USB_MSG_IN_EP
void aicwf_usb_msg_rx_submit_all_urb_(struct aic_usb_dev *usb_dev);

static void aicwf_usb_msg_rx_complete(struct urb *urb)
{
    struct aicwf_usb_buf *usb_buf = (struct aicwf_usb_buf *) urb->context;
    struct aic_usb_dev *usb_dev = usb_buf->usbdev;
    struct aicwf_rx_priv* rx_priv = usb_dev->rx_priv;
    struct sk_buff *skb = NULL;
    unsigned long flags = 0;

    skb = usb_buf->skb;
    usb_buf->skb = NULL;

    if (urb->actual_length > urb->transfer_buffer_length) {
        usb_err("usb_msg_rx len error %u/%u\n", urb->actual_length, urb->transfer_buffer_length);
        aicwf_dev_skb_free(skb);
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
		aicwf_usb_msg_rx_submit_all_urb_(usb_dev);
        //schedule_work(&usb_dev->msg_rx_urb_work);
        return;
    }

    if (urb->status != 0 || !urb->actual_length) {
        aicwf_dev_skb_free(skb);
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);

		if(urb->status < 0){
			AICWFDBG(LOGDEBUG, "%s urb->status:%d \r\n", __func__, urb->status);
			return;
		}else{
			aicwf_usb_msg_rx_submit_all_urb_(usb_dev);
			//schedule_work(&usb_dev->msg_rx_urb_work);
        	return;
		}
    }

    if (usb_dev->state == USB_UP_ST) {
        skb_put(skb, urb->actual_length);

#ifdef CONFIG_USB_RX_REASSEMBLE
        bool pkt_check = false;
        if (rx_priv->rx_msg_reassemble_skb) {
            u32 frag_len = skb->len;
            struct sk_buff *reassemble_skb = rx_priv->rx_msg_reassemble_skb;
            bool reassemble_valid = false;
            bool reassemble_done = false;
            if ((rx_priv->rx_msg_reassemble_cur_frags + 1) == rx_priv->rx_msg_reassemble_total_frags) {
                if ((rx_priv->rx_msg_reassemble_cur_len + frag_len) == rx_priv->rx_msg_reassemble_total_len) {
                    reassemble_valid = true;
                    reassemble_done = true;
                }
            } else {
                if (frag_len == AICWF_USB_MSG_MAX_PKT_SIZE) {
                    reassemble_valid = true;
                }
            }

            if (reassemble_valid) {
                memcpy((reassemble_skb->data + reassemble_skb->len), skb->data, frag_len);
                skb_put(reassemble_skb, skb->len);
                rx_priv->rx_msg_reassemble_cur_len += frag_len;
                rx_priv->rx_msg_reassemble_cur_frags++;
                aicwf_dev_skb_free(skb);
                if (reassemble_done) {
                    skb = reassemble_skb;
                    rx_priv->rx_msg_reassemble_skb = NULL;
                    rx_priv->rx_msg_reassemble_total_len = 0;
                    rx_priv->rx_msg_reassemble_cur_len = 0;
                    rx_priv->rx_msg_reassemble_total_frags = 0;
                    rx_priv->rx_msg_reassemble_cur_frags = 0;
                } else {
                    aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
                    aicwf_usb_msg_rx_submit_all_urb_(usb_dev);
                    return;
                }
            } else {
                usb_err("invalid frag msg pkt, len=%u/%u/%u, frags=%u/%u\n", frag_len,
                    rx_priv->rx_msg_reassemble_cur_len, rx_priv->rx_msg_reassemble_cur_len,
                    rx_priv->rx_msg_reassemble_cur_frags, rx_priv->rx_msg_reassemble_total_frags);
                aicwf_dev_skb_free(reassemble_skb);
                rx_priv->rx_msg_reassemble_skb = NULL;
                rx_priv->rx_msg_reassemble_total_len = 0;
                rx_priv->rx_msg_reassemble_cur_len = 0;
                rx_priv->rx_msg_reassemble_total_frags = 0;
                rx_priv->rx_msg_reassemble_cur_frags = 0;
                pkt_check = true;
            }
        } else {
            pkt_check = true;
        }

        if (pkt_check) {
            bool pkt_drop = false;
            u8 type = skb->data[2];
            u32 pkt_len = skb->data[0] | (skb->data[1] << 8);
            if ((type & USB_TYPE_CFG) != USB_TYPE_CFG) {
                usb_err("invalid msg pkt, type=0x%x, len=%u/%u\n", type, pkt_len, skb->len);;
                pkt_drop = true;
            } else {
                if (type == USB_TYPE_CFG_CMD_RSP) {
                    u32 pkt_total_len = ALIGN((pkt_len + 4), 4);
                    if ((pkt_total_len > AICWF_USB_MSG_MAX_PKT_SIZE) && (skb->len == AICWF_USB_MSG_MAX_PKT_SIZE)) {
                        AICWFDBG(LOGINFO, "reassemble msg pkt, len=%u\n", pkt_total_len);
                        struct sk_buff *reassemble_skb = __dev_alloc_skb(pkt_total_len, GFP_ATOMIC/*GFP_KERNEL*/);
                        if (reassemble_skb) {
                            memcpy(reassemble_skb->data, skb->data, skb->len);
                            skb_put(reassemble_skb, skb->len);
                            rx_priv->rx_msg_reassemble_skb = reassemble_skb;
                            rx_priv->rx_msg_reassemble_total_len = pkt_total_len;
                            rx_priv->rx_msg_reassemble_cur_len = skb->len;
                            rx_priv->rx_msg_reassemble_total_frags = ALIGN(pkt_total_len, AICWF_USB_MSG_MAX_PKT_SIZE) / AICWF_USB_MSG_MAX_PKT_SIZE;
                            rx_priv->rx_msg_reassemble_cur_frags = 1;
                        } else {
                            usb_err("reassemble msg pkt alloc fail, len=%u\n", pkt_total_len);
                        }
                        aicwf_dev_skb_free(skb);
                        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
                        aicwf_usb_msg_rx_submit_all_urb_(usb_dev);
                        return;
                    } else if (pkt_total_len != skb->len) {
                        usb_err("invalid CMD_RSP, len=%u/%u\n", pkt_len, skb->len);
                        pkt_drop = true;
                    }
                } else if (type == USB_TYPE_CFG_DATA_CFM) {
                    if (!((pkt_len == 8) && (skb->len == 12))) {
                        usb_err("invalid DATA_CFM, len=%u/%u\n", pkt_len, skb->len);
                        pkt_drop = true;
                    }
                } else {
                    usb_err("invalid msg pkt, type=0x%x, len=%u/%u\n", type, pkt_len, skb->len);
                    pkt_drop = true;
                }
            }
            if (pkt_drop) {
                aicwf_dev_skb_free(skb);
                aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
                aicwf_usb_msg_rx_submit_all_urb_(usb_dev);
                return;
            }
        }
#endif

        spin_lock_irqsave(&rx_priv->msg_rxqlock, flags);
        if(!aicwf_rxframe_enqueue(usb_dev->dev, &rx_priv->msg_rxq, skb)){
            spin_unlock_irqrestore(&rx_priv->msg_rxqlock, flags);
            usb_err("rx_priv->rxq is over flow!!!\n");
            aicwf_dev_skb_free(skb);
            return;
        }
        spin_unlock_irqrestore(&rx_priv->msg_rxqlock, flags);
        atomic_inc(&rx_priv->msg_rx_cnt);
        complete(&rx_priv->usbdev->bus_if->msg_busrx_trgg);
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
        aicwf_usb_msg_rx_submit_all_urb_(usb_dev);
        //schedule_work(&usb_dev->msg_rx_urb_work);
    } else {
        aicwf_dev_skb_free(skb);
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
    }
}
#endif

/* The submit lock closes admission before shutdown drains either RX anchor. */
static int aicwf_usb_submit_rx_urb(struct aic_usb_dev *usb_dev,
				 struct aicwf_usb_buf *usb_buf)
{
	unsigned long flags;
	int ret;
#ifdef CONFIG_PREALLOC_RX_SKB
	struct rx_buff *rx_buff;
#else
	struct sk_buff *skb;
#endif

	if (!usb_dev || !usb_buf)
		return -EINVAL;
	if (READ_ONCE(usb_dev->state) != USB_UP_ST &&
	    READ_ONCE(usb_dev->state) != USB_STARTING_ST) {
		ret = -ENETDOWN;
		goto put_buf;
	}
#ifdef CONFIG_PREALLOC_RX_SKB
	rx_buff = aicwf_prealloc_rxbuff_alloc(&usb_dev->rx_priv->rxbuff_lock);
	if (!rx_buff) {
		rx_urb_sched = true;
		ret = -ENOMEM;
		goto put_buf;
	}
	rx_buff->len = 0;
	rx_buff->start = rx_buff->data;
	rx_buff->read = rx_buff->start;
	rx_buff->end = rx_buff->data + aicwf_rxbuff_size_get();
	usb_buf->rx_buff = rx_buff;
	usb_fill_bulk_urb(usb_buf->urb, usb_dev->udev,
			  usb_dev->bulk_in_pipe, rx_buff->data,
			  aicwf_rxbuff_size_get(), aicwf_usb_rx_complete, usb_buf);
#else
	skb = __dev_alloc_skb(aicwf_usb_rx_aggr ? AICWF_USB_AGGR_MAX_PKT_SIZE :
			      aicwf_usb_max_pkt_size, GFP_ATOMIC);
	if (!skb) {
		ret = -ENOMEM;
		goto put_buf;
	}
	usb_buf->skb = skb;
	usb_fill_bulk_urb(usb_buf->urb, usb_dev->udev,
			  usb_dev->bulk_in_pipe, skb->data,
			  aicwf_usb_rx_aggr ? AICWF_USB_AGGR_MAX_PKT_SIZE :
			  aicwf_usb_max_pkt_size, aicwf_usb_rx_complete, usb_buf);
#endif
	usb_buf->usbdev = usb_dev;
	spin_lock_irqsave(&usb_dev->rx_submit_lock, flags);
	if (usb_dev->state != USB_UP_ST && usb_dev->state != USB_STARTING_ST) {
		ret = -ENETDOWN;
		spin_unlock_irqrestore(&usb_dev->rx_submit_lock, flags);
		goto free_buffer;
	}
	/* A completion may run before usb_submit_urb() returns. */
	atomic_inc(&rx_urb_cnt);
	usb_anchor_urb(usb_buf->urb, &usb_dev->rx_submitted);
	ret = usb_submit_urb(usb_buf->urb, GFP_ATOMIC);
	if (ret) {
		usb_unanchor_urb(usb_buf->urb);
		atomic_dec(&rx_urb_cnt);
	}
	spin_unlock_irqrestore(&usb_dev->rx_submit_lock, flags);
	if (!ret)
		return 0;
free_buffer:
#ifdef CONFIG_PREALLOC_RX_SKB
	aicwf_prealloc_rxbuff_free(usb_buf->rx_buff, &usb_dev->rx_priv->rxbuff_lock);
	usb_buf->rx_buff = NULL;
#else
	aicwf_dev_skb_free(usb_buf->skb);
	usb_buf->skb = NULL;
#endif
put_buf:
	aicwf_usb_rx_buf_put(usb_dev, usb_buf);
	return ret;
}

static int aicwf_usb_rx_submit_all_urb(struct aic_usb_dev *usb_dev)
{
	struct aicwf_usb_buf *usb_buf;
	int i, ret;

	/* A fast completion can recycle a buffer; never spin indefinitely. */
	for (i = 0; i < AICWF_USB_RX_URBS; i++) {
		usb_buf = aicwf_usb_rx_buf_get(usb_dev);
		if (!usb_buf)
			break;
		ret = aicwf_usb_submit_rx_urb(usb_dev, usb_buf);
		if (ret)
			return ret;
	}
	return 0;
}

#ifdef CONFIG_USB_MSG_IN_EP
static int aicwf_usb_submit_msg_rx_urb(struct aic_usb_dev *usb_dev,
				     struct aicwf_usb_buf *usb_buf)
{
	struct sk_buff *skb;
	unsigned long flags;
	int ret;

	if (!usb_dev || !usb_buf)
		return -EINVAL;
	if (READ_ONCE(usb_dev->state) != USB_UP_ST &&
	    READ_ONCE(usb_dev->state) != USB_STARTING_ST) {
		ret = -ENETDOWN;
		goto put_buf;
	}
	skb = __dev_alloc_skb(AICWF_USB_MSG_MAX_PKT_SIZE, GFP_ATOMIC);
	if (!skb) {
		ret = -ENOMEM;
		goto put_buf;
	}
	usb_buf->skb = skb;
	usb_buf->usbdev = usb_dev;
	usb_fill_bulk_urb(usb_buf->urb, usb_dev->udev, usb_dev->msg_in_pipe,
			  skb->data, AICWF_USB_MSG_MAX_PKT_SIZE,
			  aicwf_usb_msg_rx_complete, usb_buf);
	spin_lock_irqsave(&usb_dev->rx_submit_lock, flags);
	if (usb_dev->state != USB_UP_ST && usb_dev->state != USB_STARTING_ST) {
		ret = -ENETDOWN;
		spin_unlock_irqrestore(&usb_dev->rx_submit_lock, flags);
		goto free_buffer;
	}
	usb_anchor_urb(usb_buf->urb, &usb_dev->msg_rx_submitted);
	ret = usb_submit_urb(usb_buf->urb, GFP_ATOMIC);
	if (ret)
		usb_unanchor_urb(usb_buf->urb);
	spin_unlock_irqrestore(&usb_dev->rx_submit_lock, flags);
	if (!ret)
		return 0;
free_buffer:
	aicwf_dev_skb_free(usb_buf->skb);
	usb_buf->skb = NULL;
put_buf:
	aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
	return ret;
}

static int aicwf_usb_msg_rx_submit_all_urb(struct aic_usb_dev *usb_dev)
{
	struct aicwf_usb_buf *usb_buf;
	int i, ret;

	for (i = 0; i < AICWF_USB_MSG_RX_URBS; i++) {
		usb_buf = aicwf_usb_msg_rx_buf_get(usb_dev);
		if (!usb_buf)
			break;
		ret = aicwf_usb_submit_msg_rx_urb(usb_dev, usb_buf);
		if (ret)
			return ret;
	}
	return 0;
}

void aicwf_usb_msg_rx_submit_all_urb_(struct aic_usb_dev *usb_dev)
{
	/* Preparation has its own bounded pass; callbacks cannot restart it. */
	if (READ_ONCE(usb_dev->state) == USB_UP_ST)
		aicwf_usb_msg_rx_submit_all_urb(usb_dev);
}
#endif

void aicwf_usb_rx_submit_all_urb_(struct aic_usb_dev *usb_dev)
{
	if (READ_ONCE(usb_dev->state) == USB_UP_ST)
		aicwf_usb_rx_submit_all_urb(usb_dev);
}

static int aicwf_usb_rx_prepare(struct aic_usb_dev *usb_dev)
{
	return aicwf_usb_rx_submit_all_urb(usb_dev);
}

#ifdef CONFIG_USB_MSG_IN_EP
static int aicwf_usb_msg_rx_prepare(struct aic_usb_dev *usb_dev)
{
	return aicwf_usb_msg_rx_submit_all_urb(usb_dev);
}
#endif

#ifdef CONFIG_USB_TX_AGGR
int aicwf_usb_send_pkt(struct aic_usb_dev *usb_dev, u8 *buf, uint buf_len)
{
    int ret = 0;
    struct aicwf_usb_buf *usb_buf;
    unsigned long flags;
    bool need_cfm = false;

    if (usb_dev->state != USB_UP_ST) {
        AICWFDBG(LOGERROR, "usb state is not up!\n");
        return -EIO;
    }

    usb_buf = aicwf_usb_tx_dequeue(usb_dev, &usb_dev->tx_free_list,
                        &usb_dev->tx_free_count, &usb_dev->tx_free_lock);
    if (!usb_buf) {
        AICWFDBG(LOGERROR, "free:%d, post:%d\n", usb_dev->tx_free_count, usb_dev->tx_post_count);
        ret = -ENOMEM;
        goto flow_ctrl;
    }

    usb_buf->skb = (struct sk_buff *)buf;
    usb_buf->usbdev = usb_dev;
    if (need_cfm)
        usb_buf->cfm = true;
    else
        usb_buf->cfm = false;
    AICWFDBG(LOGERROR, "%s len %d\n", __func__, buf_len);
    print_hex_dump(KERN_ERR, "buf  ", DUMP_PREFIX_NONE, 16, 1, &buf[0], 32, false);
    usb_fill_bulk_urb(usb_buf->urb, usb_dev->udev, usb_dev->bulk_out_pipe,
                buf, buf_len, aicwf_usb_tx_complete, usb_buf);
    usb_buf->urb->transfer_flags |= URB_ZERO_PACKET;

    aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_post_list, usb_buf,
                    &usb_dev->tx_post_count, &usb_dev->tx_post_lock);
    ret = 0;

    flow_ctrl:
    spin_lock_irqsave(&usb_dev->tx_flow_lock, flags);
    if (usb_dev->tx_free_count < AICWF_USB_TX_LOW_WATER) {
        usb_dev->tbusy = true;
        aicwf_usb_tx_flowctrl(usb_dev->rwnx_hw, true);
    }
    spin_unlock_irqrestore(&usb_dev->tx_flow_lock, flags);

    return ret;
}

int aicwf_usb_aggr(struct aicwf_tx_priv *tx_priv, struct sk_buff *pkt)
{
    struct rwnx_txhdr *txhdr = (struct rwnx_txhdr *)pkt->data;
    u8 usb_header[8];
    u8 adjust_str[4] = {0, 0, 0, 0};
    u32 curr_len = 0;
    int allign_len = 0;
    u32 data_len = (pkt->len - sizeof(struct rwnx_txhdr) + sizeof(struct txdesc_api)) + 4;

    usb_header[0] =(data_len & 0xff);
    usb_header[1] =((data_len >> 8)&0x0f);
    usb_header[2] =(data_len & 0xff);
    usb_header[3] =((data_len >> 8)&0x0f);

    usb_header[4] =(data_len & 0xff);
    usb_header[5] =((data_len >> 8)&0x0f);
    usb_header[6] = 0x01; //data
    usb_header[7] = 0; //reserved

    memcpy(tx_priv->tail, (u8 *)&usb_header, sizeof(usb_header));
    tx_priv->tail += sizeof(usb_header);
    //payload
    memcpy(tx_priv->tail, (u8 *)(long)&txhdr->sw_hdr->desc, sizeof(struct txdesc_api));
    tx_priv->tail += sizeof(struct txdesc_api); //hostdesc
    memcpy(tx_priv->tail, (u8 *)((u8 *)txhdr + txhdr->sw_hdr->headroom), pkt->len-txhdr->sw_hdr->headroom);
    tx_priv->tail += (pkt->len - txhdr->sw_hdr->headroom);

    //word alignment
    curr_len = tx_priv->tail - tx_priv->head;
    if (curr_len & (TX_ALIGNMENT - 1)) {
        allign_len = roundup(curr_len, TX_ALIGNMENT)-curr_len;
        memcpy(tx_priv->tail, adjust_str, allign_len);
        tx_priv->tail += allign_len;
    }

    tx_priv->aggr_buf->dev = pkt->dev;

    if(!txhdr->sw_hdr->need_cfm) {
        kmem_cache_free(txhdr->sw_hdr->rwnx_vif->rwnx_hw->sw_txhdr_cache, txhdr->sw_hdr);
        skb_pull(pkt, txhdr->sw_hdr->headroom);
        consume_skb(pkt);
    }

    atomic_inc(&tx_priv->aggr_count);
    return 0;
}

int aicwf_usb_send(struct aicwf_tx_priv *tx_priv)
{
    struct sk_buff *pkt;
    struct sk_buff *tx_buf;
    struct aic_usb_dev *usbdev = tx_priv->usbdev;
    struct aicwf_usb_buf *usb_buf;
    u8* buf;
    int ret = 0;
    int curr_len = 0;
    unsigned long flags;

    if (aicwf_is_framequeue_empty(&tx_priv->txq)) {
        ret = -1;
        AICWFDBG(LOGERROR, "no buf to send\n");
        return ret;
    }

    if (usbdev->state != USB_UP_ST) {
        AICWFDBG(LOGERROR, "usb state is not up!\n");
        ret = -ENODEV;
        return ret;
    }
    usb_buf = aicwf_usb_tx_dequeue(usbdev, &usbdev->tx_free_list,
                        &usbdev->tx_free_count, &usbdev->tx_free_lock);
    if (!usb_buf) {
        AICWFDBG(LOGERROR, "free:%d, post:%d\n", usbdev->tx_free_count, usbdev->tx_post_count);
        ret = -ENOMEM;
        return ret;
    }

    usb_buf->aggr_cnt = 0;
    spin_lock_bh(&usbdev->tx_priv->txdlock);
    tx_priv->head = usb_buf->skb->data;
    tx_priv->tail = usb_buf->skb->data;

    while (!aicwf_is_framequeue_empty(&usbdev->tx_priv->txq)) {
        if (usbdev->state != USB_UP_ST) {
            AICWFDBG(LOGERROR, "usb state is not up, break!\n");
            ret = -ENODEV;
            break;
        }

        if (usb_buf->aggr_cnt == 10) {
            break;
        }
        spin_lock_bh(&usbdev->tx_priv->txqlock);
        pkt = aicwf_frame_dequeue(&usbdev->tx_priv->txq);
        if (pkt == NULL) {
            AICWFDBG(LOGERROR, "txq no pkt\n");
            spin_unlock_bh(&usbdev->tx_priv->txqlock);
            ret = -1;
            return ret;
        }
        atomic_dec(&usbdev->tx_priv->tx_pktcnt);
        spin_unlock_bh(&usbdev->tx_priv->txqlock);
        if(tx_priv==NULL || tx_priv->tail==NULL || pkt==NULL) {
            AICWFDBG(LOGERROR, "null error\n");
        }
        aicwf_usb_aggr(tx_priv, pkt);
        usb_buf->aggr_cnt++;
    }

    tx_buf = usb_buf->skb;
    buf = tx_buf->data;

    curr_len = tx_priv->tail - tx_priv->head;

    AICWFDBG(LOGTRACE, "%s len %d, cnt %d\n", __func__,curr_len, usb_buf->aggr_cnt);
    tx_buf->len = tx_priv->tail - tx_priv->head;
    spin_unlock_bh(&usbdev->tx_priv->txdlock);
    usb_fill_bulk_urb(usb_buf->urb, usbdev->udev, usbdev->bulk_out_pipe,
                buf, curr_len, aicwf_usb_tx_complete, usb_buf);
    usb_buf->urb->transfer_flags |= URB_ZERO_PACKET;

    aicwf_usb_tx_queue(usbdev, &usbdev->tx_post_list, usb_buf,
                    &usbdev->tx_post_count, &usbdev->tx_post_lock);

    flow_ctrl:
    spin_lock_irqsave(&usbdev->tx_flow_lock, flags);
    if (usbdev->tx_free_count < AICWF_USB_TX_LOW_WATER) {
        usbdev->tbusy = true;
        aicwf_usb_tx_flowctrl(usbdev->rwnx_hw, true);
    }
    spin_unlock_irqrestore(&usbdev->tx_flow_lock, flags);

    return ret;
}

#endif

static void aicwf_usb_tx_process(struct aic_usb_dev *usb_dev)
{
	struct aicwf_usb_buf *usb_buf;
	int ret = 0;

#ifdef CONFIG_USB_TX_AGGR
	if (!aicwf_is_framequeue_empty(&usb_dev->tx_priv->txq)) {
		if (aicwf_usb_send(usb_dev->tx_priv))
			AICWFDBG(LOGERROR, "%s no buf send\n", __func__);
	}
#endif

	while (!list_empty(&usb_dev->tx_post_list)) {

		if (usb_dev->state != USB_UP_ST || READ_ONCE(usb_dev->tx_stopping))
			return;

		usb_buf = aicwf_usb_tx_dequeue(usb_dev, &usb_dev->tx_post_list,
						&usb_dev->tx_post_count, &usb_dev->tx_post_lock);
		if (!usb_buf) {
			usb_err("can not get usb_buf from tx_post_list!\n");
			return;
		}
		usb_anchor_urb(usb_buf->urb, &usb_dev->tx_submitted);
		ret = usb_submit_urb(usb_buf->urb, GFP_ATOMIC);
		if (ret) {
			usb_unanchor_urb(usb_buf->urb);
			AICWFDBG(LOGERROR, "aicwf_usb_bus_tx usb_submit_urb FAILED err:%d\n", ret);
			#ifdef CONFIG_USB_TX_AGGR
			aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_post_list, usb_buf,
					&usb_dev->tx_post_count, &usb_dev->tx_post_lock);
			break;
			#else
			goto fail;
			#endif
		}

		continue;
#ifndef CONFIG_USB_TX_AGGR
fail:
		aicwf_usb_tx_finish(usb_buf, ret);
#endif
	}
}

#ifdef CONFIG_TX_TASKLET
void aicwf_tasklet_tx_process(struct aic_usb_dev *usb_dev){
	aicwf_usb_tx_process(usb_dev);
}
#endif

static inline void aic_thread_wait_stop(void)
{
#if 1// PLATFORM_LINUX
	#if 0
	while (!kthread_should_stop())
		rtw_msleep_os(10);
	#else
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	#endif
#endif
}


int usb_bustx_thread(void *data)
{
    struct aicwf_bus *bus = (struct aicwf_bus *)data;
    struct aic_usb_dev *usbdev = bus->bus_priv.usb;
    int set_cpu_ret = 0;

#ifdef CONFIG_THREAD_INFO_IN_TASK
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0))
	AICWFDBG(LOGINFO, "%s the cpu is:%d\n", __func__, current->thread_info.cpu);
#else
    AICWFDBG(LOGINFO, "%s the cpu is:%d\n", __func__, current->cpu);
#endif
#endif
    set_cpu_ret = set_cpus_allowed_ptr(current, cpumask_of(1));
#ifdef CONFIG_THREAD_INFO_IN_TASK
    AICWFDBG(LOGINFO, "%s set_cpu_ret is:%d\n", __func__, set_cpu_ret);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0))
	AICWFDBG(LOGINFO, "%s change cpu to:%d\n", __func__, current->thread_info.cpu);
#else
    AICWFDBG(LOGINFO, "%s change cpu to:%d\n", __func__, current->cpu);
#endif
#endif


#ifdef CONFIG_TXRX_THREAD_PRIO
	if (bustx_thread_prio > 0) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
        sched_set_fifo_low(current);
#else
        struct sched_param param;
        param.sched_priority = (bustx_thread_prio < MAX_RT_PRIO)?bustx_thread_prio:(MAX_RT_PRIO-1);
        sched_setscheduler(current, SCHED_FIFO, &param);
#endif
	}
#endif
	AICWFDBG(LOGINFO, "%s the policy of current thread is:%d\n", __func__, current->policy);
	AICWFDBG(LOGINFO, "%s the rt_priority of current thread is:%d\n", __func__, current->rt_priority);
	AICWFDBG(LOGINFO, "%s the current pid is:%d\n", __func__, current->pid);


	while (!kthread_should_stop()) {
		if (wait_for_completion_interruptible(&bus->bustx_trgg))
			continue;
		if (kthread_should_stop() || READ_ONCE(usbdev->tx_stopping) ||
		    usbdev->bus_if->state == BUS_DOWN_ST)
			break;
#ifdef CONFIG_USB_TX_AGGR
		if (usbdev->tx_post_count || !aicwf_is_framequeue_empty(&usbdev->tx_priv->txq))
#else
		if (usbdev->tx_post_count)
#endif
			aicwf_usb_tx_process(usbdev);
	}

	aic_thread_wait_stop();
	AICWFDBG(LOGINFO, "usb bustx thread stop\n");

    return 0;
}

int usb_busrx_thread(void *data)
{
    struct aicwf_rx_priv *rx_priv = (struct aicwf_rx_priv *)data;
    struct aicwf_bus *bus_if = rx_priv->usbdev->bus_if;
    int set_cpu_ret = 0;
    
#ifdef CONFIG_THREAD_INFO_IN_TASK
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0))
	AICWFDBG(LOGINFO, "%s the cpu is:%d\n", __func__, current->thread_info.cpu);
#else
    AICWFDBG(LOGINFO, "%s the cpu is:%d\n", __func__, current->cpu);
#endif
#endif
    set_cpu_ret = set_cpus_allowed_ptr(current, cpumask_of(1));
#ifdef CONFIG_THREAD_INFO_IN_TASK
    AICWFDBG(LOGINFO, "%s set_cpu_ret is:%d\n", __func__, set_cpu_ret);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0))
	AICWFDBG(LOGINFO, "%s change cpu to:%d\n", __func__, current->thread_info.cpu);
#else
    AICWFDBG(LOGINFO, "%s change cpu to:%d\n", __func__, current->cpu);
#endif
#endif

#ifdef CONFIG_TXRX_THREAD_PRIO
	if (busrx_thread_prio > 0) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
        sched_set_fifo_low(current);
#else
		struct sched_param param;
		param.sched_priority = (busrx_thread_prio < MAX_RT_PRIO)?busrx_thread_prio:(MAX_RT_PRIO-1);
		sched_setscheduler(current, SCHED_FIFO, &param);
#endif
	}
#endif
	AICWFDBG(LOGINFO, "%s the policy of current thread is:%d\n", __func__, current->policy);
	AICWFDBG(LOGINFO, "%s the rt_priority of current thread is:%d\n", __func__, current->rt_priority);
	AICWFDBG(LOGINFO, "%s the current pid is:%d\n", __func__, current->pid);

    while (1) {
#if 0
        if(kthread_should_stop()) {
            usb_err("usb busrx thread stop 2\n");
            break;
        }
#endif
		//rx_priv->rx_thread_working = 0;//AIDEN
        if (!wait_for_completion_interruptible(&bus_if->busrx_trgg)) {
            if(bus_if->state == BUS_DOWN_ST){
				AICWFDBG(LOGINFO, "usb busrx thread will to stop\n");
				break;
            }
			//rx_priv->rx_thread_working = 1;//AIDEN
            aicwf_process_rxframes(rx_priv);
        }
    }

	aic_thread_wait_stop();
	AICWFDBG(LOGINFO, "usb busrx thread stop\n");

    return 0;
}

#ifdef CONFIG_USB_MSG_IN_EP
int usb_msg_busrx_thread(void *data)
{
    struct aicwf_rx_priv *rx_priv = (struct aicwf_rx_priv *)data;
    struct aicwf_bus *bus_if = rx_priv->usbdev->bus_if;

#ifdef CONFIG_TXRX_THREAD_PRIO
	if (busrx_thread_prio > 0) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
        sched_set_fifo_low(current);
#else
        struct sched_param param;
        param.sched_priority = (busrx_thread_prio < MAX_RT_PRIO)?busrx_thread_prio:(MAX_RT_PRIO-1);
        sched_setscheduler(current, SCHED_FIFO, &param);
#endif
	}
#endif
	AICWFDBG(LOGINFO, "%s the policy of current thread is:%d\n", __func__, current->policy);
	AICWFDBG(LOGINFO, "%s the rt_priority of current thread is:%d\n", __func__, current->rt_priority);
	AICWFDBG(LOGINFO, "%s the current pid is:%d\n", __func__, current->pid);



    while (1) {
#if 0
        if(kthread_should_stop()) {
            usb_err("usb msg busrx thread stop\n");
            break;
        }
#endif
        if (!wait_for_completion_interruptible(&bus_if->msg_busrx_trgg)) {
            if(bus_if->state == BUS_DOWN_ST){
		AICWFDBG(LOGINFO, "usb msg busrx thread will to stop\n");
                break;
	    }
            aicwf_process_msg_rxframes(rx_priv);
        }
    }

	aic_thread_wait_stop();
	AICWFDBG(LOGINFO, "usb msg busrx thread stop\n");

    return 0;
}
#endif


static void aicwf_usb_send_msg_complete(struct urb *urb)
{
	struct aic_usb_dev *usb_dev = urb->context;

	WRITE_ONCE(usb_dev->msg_status, urb->status);
	WRITE_ONCE(usb_dev->msg_actual_length, urb->actual_length);
	/* Publish the result before the sender observes completion. */
	smp_store_release(&usb_dev->msg_finished, true);
	wake_up(&usb_dev->msg_wait);
}

/* The command worker serializes access to the shared message buffer and URB. */
static int aicwf_usb_bus_txmsg(struct device *dev, u8 *buf, u32 len)
{
	int ret;
	struct aicwf_bus *bus_if = dev_get_drvdata(dev);
	struct aic_usb_dev *usb_dev;

	if (!bus_if || !bus_if->bus_priv.usb)
		return -ENODEV;
	usb_dev = bus_if->bus_priv.usb;

	if (READ_ONCE(usb_dev->state) != USB_UP_ST)
		return -ENETDOWN;

	if (buf == NULL || len == 0 || usb_dev->msg_out_urb == NULL)
		return -EINVAL;

	WRITE_ONCE(usb_dev->msg_status, -EINPROGRESS);
	WRITE_ONCE(usb_dev->msg_actual_length, 0);
	WRITE_ONCE(usb_dev->msg_finished, false);

#ifdef CONFIG_USB_MSG_OUT_EP
	if (usb_dev->msg_out_pipe) {
		usb_fill_bulk_urb(usb_dev->msg_out_urb,
			usb_dev->udev,
			usb_dev->msg_out_pipe,
			buf, len, (usb_complete_t) aicwf_usb_send_msg_complete, usb_dev);
	} else {
		usb_fill_bulk_urb(usb_dev->msg_out_urb,
			usb_dev->udev,
			usb_dev->bulk_out_pipe,
			buf, len, (usb_complete_t) aicwf_usb_send_msg_complete, usb_dev);
	}
#else
	usb_fill_bulk_urb(usb_dev->msg_out_urb,
		usb_dev->udev,
		usb_dev->bulk_out_pipe,
		buf, len, (usb_complete_t) aicwf_usb_send_msg_complete, usb_dev);
#endif
	#if defined CONFIG_USB_NO_TRANS_DMA_MAP
	usb_dev->msg_out_urb->transfer_dma = usb_dev->cmd_dma_trans_addr;
	usb_dev->msg_out_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	#endif
#ifdef CONFIG_USE_USB_ZERO_PACKET
	usb_dev->msg_out_urb->transfer_flags |= URB_ZERO_PACKET;
#endif
	ret = usb_submit_urb(usb_dev->msg_out_urb, GFP_ATOMIC);
	if (ret) {
		usb_err("usb_submit_urb failed %d\n", ret);
		return ret;
	}

	ret = wait_event_timeout(usb_dev->msg_wait,
		/* Pair with the completion callback's result publication. */
		smp_load_acquire(&usb_dev->msg_finished),
		msecs_to_jiffies(CMD_TX_TIMEOUT));
	if (!ret) {
		usb_kill_urb(usb_dev->msg_out_urb);
		usb_err("Txmsg wait timed out\n");
		return -ETIMEDOUT;
	}

	ret = READ_ONCE(usb_dev->msg_status);
	if (ret)
		return ret < 0 ? ret : -EIO;
	if (READ_ONCE(usb_dev->msg_actual_length) != len)
		return -EREMOTEIO;

	return 0;
}


static void aicwf_usb_free_urb(struct list_head *q, spinlock_t *qlock)
{
    struct aicwf_usb_buf *usb_buf, *tmp;
    unsigned long flags;

    spin_lock_irqsave(qlock, flags);
    list_for_each_entry_safe(usb_buf, tmp, q, list) {
        spin_unlock_irqrestore(qlock, flags);
        if (!usb_buf->urb) {
            usb_err("bad usb_buf\n");
            spin_lock_irqsave(qlock, flags);
            break;
        }
        #ifdef CONFIG_USB_TX_AGGR
        if (usb_buf->skb) {
            dev_kfree_skb(usb_buf->skb);
        }
        #endif
        usb_free_urb(usb_buf->urb);
        #if defined CONFIG_USB_NO_TRANS_DMA_MAP
        // free dma buf if needed
        if (usb_buf->data_buf) {
            #if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35))
            usb_free_coherent(usb_buf->usbdev->udev, DATA_BUF_MAX, usb_buf->data_buf, usb_buf->data_dma_trans_addr);
            #else
            usb_buffer_free(usb_buf->usbdev->udev, DATA_BUF_MAX, usb_buf->data_buf, usb_buf->data_dma_trans_addr);
            #endif
            usb_buf->data_buf = NULL;
            usb_buf->data_dma_trans_addr = 0x0;
        }
        #endif
        list_del_init(&usb_buf->list);
        spin_lock_irqsave(qlock, flags);
    }
    spin_unlock_irqrestore(qlock, flags);
}

static int aicwf_usb_alloc_rx_urb(struct aic_usb_dev *usb_dev)
{
    int i;

	AICWFDBG(LOGINFO, "%s AICWF_USB_RX_URBS:%d \r\n", __func__, AICWF_USB_RX_URBS);
    for (i = 0; i < AICWF_USB_RX_URBS; i++) {
        struct aicwf_usb_buf *usb_buf = &usb_dev->usb_rx_buf[i];

        usb_buf->usbdev = usb_dev;
        usb_buf->urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!usb_buf->urb) {
            usb_err("could not allocate rx data urb\n");
            goto err;
        }
        #if defined CONFIG_USB_NO_TRANS_DMA_MAP
        // dma buf unused
        usb_buf->data_buf = NULL;
        usb_buf->data_dma_trans_addr = 0x0;
        #endif
        list_add_tail(&usb_buf->list, &usb_dev->rx_free_list);
    }
    return 0;

err:
    aicwf_usb_free_urb(&usb_dev->rx_free_list, &usb_dev->rx_free_lock);
    return -ENOMEM;
}

static int aicwf_usb_alloc_tx_urb(struct aic_usb_dev *usb_dev)
{
    int i;

	AICWFDBG(LOGINFO, "%s AICWF_USB_TX_URBS:%d \r\n", __func__, AICWF_USB_TX_URBS);
    for (i = 0; i < AICWF_USB_TX_URBS; i++) {
        struct aicwf_usb_buf *usb_buf = &usb_dev->usb_tx_buf[i];

        usb_buf->usbdev = usb_dev;
        usb_buf->urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!usb_buf->urb) {
            usb_err("could not allocate tx data urb\n");
            goto err;
        }
        #ifdef CONFIG_USB_TX_AGGR
        usb_buf->skb = dev_alloc_skb(MAX_USB_AGGR_TXPKT_LEN);
        #endif
        #if defined CONFIG_USB_NO_TRANS_DMA_MAP
        // alloc dma buf
        #if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35))
        usb_buf->data_buf = usb_alloc_coherent(usb_dev->udev, DATA_BUF_MAX, (in_interrupt() ? GFP_ATOMIC : GFP_KERNEL), &usb_buf->data_dma_trans_addr);
        #else
        usb_buf->data_buf = usb_buffer_alloc(usb_dev->udev, DATA_BUF_MAX, (in_interrupt() ? GFP_ATOMIC : GFP_KERNEL), &usb_buf->data_dma_trans_addr);
        #endif
        if (usb_buf->data_buf == NULL) {
            usb_err("could not allocate tx data dma buf\n");
            goto err;
        }
        #endif
        list_add_tail(&usb_buf->list, &usb_dev->tx_free_list);
        (usb_dev->tx_free_count)++;
    }
    return 0;

err:
    aicwf_usb_free_urb(&usb_dev->tx_free_list, &usb_dev->tx_free_lock);
    return -ENOMEM;
}

#ifdef CONFIG_USB_MSG_IN_EP
static int aicwf_usb_alloc_msg_rx_urb(struct aic_usb_dev *usb_dev)
{
    int i;

    AICWFDBG(LOGINFO, "%s AICWF_USB_MSG_RX_URBS:%d \r\n", __func__, AICWF_USB_MSG_RX_URBS);

    for (i = 0; i < AICWF_USB_MSG_RX_URBS; i++) {
        struct aicwf_usb_buf *usb_buf = &usb_dev->usb_msg_rx_buf[i];

        usb_buf->usbdev = usb_dev;
        usb_buf->urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!usb_buf->urb) {
            usb_err("could not allocate rx data urb\n");
            goto err;
        }
        list_add_tail(&usb_buf->list, &usb_dev->msg_rx_free_list);
    }
    return 0;

err:
    aicwf_usb_free_urb(&usb_dev->msg_rx_free_list, &usb_dev->msg_rx_free_lock);
    return -ENOMEM;
}
#endif

static void aicwf_usb_state_change(struct aic_usb_dev *usb_dev, int state)
{
	unsigned long flags;

	spin_lock_irqsave(&usb_dev->rx_submit_lock, flags);
	WRITE_ONCE(usb_dev->state, state);
	if (state != USB_UP_ST)
		WRITE_ONCE(usb_dev->rx_prepare_ready, false);
	WRITE_ONCE(usb_dev->bus_if->state,
		   state == USB_UP_ST ? BUS_UP_ST : BUS_DOWN_ST);
	spin_unlock_irqrestore(&usb_dev->rx_submit_lock, flags);
}

int align_param = 8;
module_param(align_param, int, 0660);

static void usb_tx_flow_ctrl(struct rwnx_txhdr *txhdr, struct aic_usb_dev *usb_dev, struct rwnx_hw *rwnx_hw)
{
#ifdef CONFIG_PER_STA_FC
	struct rwnx_sta *sta;
	u8 sta_idx;
	int srcu_idx;
	unsigned long flags;

	//printk("txdata: sta %d\n", txhdr->sw_hdr->desc.host.staid);
	sta_idx = txhdr->sw_hdr->desc.host.staid;
	if(sta_idx < NX_REMOTE_STA_MAX && !(txhdr->sw_hdr->desc.host.flags & TXU_CNTRL_MGMT)) {
		struct rwnx_vif *vif = NULL;
		sta = &rwnx_hw->sta_table[sta_idx];
		srcu_idx = srcu_read_lock(&rwnx_hw->vif_readers);
		vif = rwnx_vif_lookup(rwnx_hw, READ_ONCE(sta->vif_idx));
		spin_lock_irqsave(&usb_dev->tx_flow_lock, flags);
		atomic_inc(&rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt);
		//printk("sta %d pending %d >= 64, flowctrl=%d\n", sta->sta_idx, sta->tx_pending_cnt, sta->flowctrl);
		if (vif && rwnx_sta_is_valid(sta) &&
		    RWNX_VIF_TYPE(vif) == NL80211_IFTYPE_AP) {
			if((atomic_read(&rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt) >= AICWF_USB_FC_PERSTA_HIGH_WATER && rwnx_hw->sta_flowctrl[sta_idx].flowctrl == 0) ||
										rwnx_hw->sta_flowctrl[sta_idx].flowctrl) {
				//AICWFDBG(LOGDEBUG, "sta 0x%x:0x%x, %d pending %d, stop\n", sta->mac_addr[4], sta->mac_addr[5], sta->sta_idx, atomic_read(&rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt));
				if(!usb_dev->tbusy)
					rwnx_stop_sta_all_queues((void *)sta, usb_dev->rwnx_hw);
				rwnx_hw->sta_flowctrl[sta_idx].flowctrl = 1;
			}
		}
		spin_unlock_irqrestore(&usb_dev->tx_flow_lock, flags);
		srcu_read_unlock(&rwnx_hw->vif_readers, srcu_idx);
	}
#endif
}

#ifdef CONFIG_USB_TX_AGGR
static int aicwf_usb_bus_txdata(struct device *dev, struct sk_buff *pkt)
{
    uint prio;
    int ret = -EBADE;
    struct aicwf_bus *bus_if = dev_get_drvdata(dev);
    struct aic_usb_dev *usbdev = bus_if->bus_priv.usb;

    //printk("%s\n", __func__);
    prio = (pkt->priority & 0x7);
    spin_lock_bh(&usbdev->tx_priv->txqlock);
    if (!aicwf_frame_enq(usbdev->dev, &usbdev->tx_priv->txq, pkt, prio)) {
        aicwf_dev_skb_free(pkt);
        spin_unlock_bh(&usbdev->tx_priv->txqlock);
        return -ENOSR;
    } else {
        ret = 0;
    }

    if (bus_if->state != BUS_UP_ST) {
        usb_err("bus_if stopped\n");
        spin_unlock_bh(&usbdev->tx_priv->txqlock);
        return -1;
    }

    atomic_inc(&usbdev->tx_priv->tx_pktcnt);
    spin_unlock_bh(&usbdev->tx_priv->txqlock);
    complete(&bus_if->bustx_trgg);

    return ret;
}

#else
/* Called with tx_lock held; consume skb on both success and failure. */
static int aicwf_usb_bus_txdata(struct device *dev, struct sk_buff *skb)
{
	struct aicwf_bus *bus = dev_get_drvdata(dev);
	struct aic_usb_dev *usb = bus->bus_priv.usb;
	struct rwnx_hw *hw = usb->rwnx_hw;
	struct rwnx_sw_txhdr *sw = ((struct rwnx_txhdr *)skb->data)->sw_hdr;
	struct aicwf_usb_buf *usb_buf = NULL;
	unsigned int payload_len, wire_len, transfer_len, extra = 0;
	unsigned long flags;
	u8 *buf, *wire;
	int ret = -ENOMEM;
#ifdef CONFIG_USB_ALIGN_DATA
	unsigned int alignment = READ_ONCE(align_param);
#ifdef CONFIG_USB_NO_TRANS_DMA_MAP
#error "CONFIG_USB_NO_TRANS_DMA_MAP with CONFIG_USB_ALIGN_DATA is not supported"
#endif
#endif

	lockdep_assert_held(&hw->tx_lock);
	if (usb->state != USB_UP_ST || READ_ONCE(usb->tx_stopping)) {
		ret = -ESHUTDOWN;
		goto drop;
	}
	/* The bus header has a 12-bit length, independently of the TX descriptor. */
	if (sw->headroom > skb->len ||
	    sw->headroom < sizeof(struct txdesc_api) + 4) {
		ret = -EINVAL;
		goto drop;
	}
	payload_len = skb->len - sw->headroom;
	if (payload_len > 0xfff - sizeof(struct txdesc_api) - 4) {
		ret = -EMSGSIZE;
		goto drop;
	}
	wire_len = 4 + sizeof(struct txdesc_api) + payload_len;
	if (sw->need_cfm)
		wire_len = ALIGN(wire_len, TX_ALIGNMENT);
	if (wire_len > 0xfff) {
		ret = -EMSGSIZE;
		goto drop;
	}
#ifndef CONFIG_USE_USB_ZERO_PACKET
	if (!(wire_len % usb_maxpacket(usb->udev, usb->bulk_out_pipe)))
		extra = 1;
#endif
	transfer_len = wire_len + extra;
#ifdef CONFIG_USB_NO_TRANS_DMA_MAP
	if (transfer_len > DATA_BUF_MAX) {
		ret = -EMSGSIZE;
		goto drop;
	}
#endif
#ifdef CONFIG_USB_ALIGN_DATA
	if (!is_power_of_2(alignment) || alignment > PAGE_SIZE) {
		ret = -EINVAL;
		goto drop;
	}
#endif
	usb_buf = aicwf_usb_tx_dequeue(usb, &usb->tx_free_list,
				     &usb->tx_free_count, &usb->tx_free_lock);
	if (!usb_buf)
		goto drop;
	usb_buf->skb = NULL;
	usb_buf->tx_data = NULL;
	usb_buf->usb_align_data = NULL;
	usb_buf->cfm = sw->need_cfm;
	usb_buf->cfm_tag = sw->desc.host.status_desc_addr & 0x3fffffff;

	/* Complete every fallible preparation step before relinquishing sw. */
	if (skb_linearize(skb))
		goto recycle;
	if (!sw->need_cfm && extra && skb_tailroom(skb) < extra &&
	    pskb_expand_head(skb, 0, extra, GFP_ATOMIC))
		goto recycle;
#ifdef CONFIG_USB_NO_TRANS_DMA_MAP
	buf = usb_buf->data_buf;
#else
	if (sw->need_cfm) {
		usb_buf->tx_data = kzalloc(transfer_len, GFP_ATOMIC);
		if (!usb_buf->tx_data)
			goto recycle;
		buf = usb_buf->tx_data;
	} else {
		buf = skb->data + sw->headroom - sizeof(struct txdesc_api) - 4;
	}
#endif
	wire = buf;
#ifdef CONFIG_USB_ALIGN_DATA
	if (!IS_ALIGNED((unsigned long)buf, alignment)) {
		usb_buf->usb_align_data = kmalloc(transfer_len + alignment - 1,
						 GFP_ATOMIC);
		if (!usb_buf->usb_align_data)
			goto recycle;
		wire = PTR_ALIGN(usb_buf->usb_align_data, alignment);
	}
#endif
	usb_tx_flow_ctrl((struct rwnx_txhdr *)skb->data, usb, hw);
	if (sw->need_cfm) {
		memcpy(buf + 4, &sw->desc, sizeof(sw->desc));
		memcpy(buf + 4 + sizeof(sw->desc), skb->data + sw->headroom,
		       payload_len);
		memset(buf + 4 + sizeof(sw->desc) + payload_len, 0,
		       transfer_len - 4 - sizeof(sw->desc) - payload_len);
	} else {
		skb_pull(skb, sw->headroom);
		memcpy(skb_push(skb, sizeof(sw->desc)), &sw->desc, sizeof(sw->desc));
		skb_push(skb, 4);
		if (extra)
			*(u8 *)skb_put(skb, 1) = 0;
#ifdef CONFIG_USB_NO_TRANS_DMA_MAP
		memcpy(buf, skb->data, transfer_len);
#endif
		usb_buf->skb = skb;
		/* No radio confirmation follows: retire host bookkeeping now. */
		rwnx_txq_confirm_any(hw, sw->txq, &hw->hwq[sw->hw_queue], sw);
		sw->rwnx_vif->net_stats.tx_packets++;
		sw->rwnx_vif->net_stats.tx_bytes += sw->frame_len;
		hw->stats.last_tx = jiffies;
		kmem_cache_free(hw->sw_txhdr_cache, sw);
	}
	buf[0] = wire_len & 0xff;
	buf[1] = (wire_len >> 8) & 0x0f;
	buf[2] = 0x01;
	buf[3] = 0;
	if (wire != buf)
		memcpy(wire, buf, transfer_len);
	usb_fill_bulk_urb(usb_buf->urb, usb->udev, usb->bulk_out_pipe,
			  wire, transfer_len, aicwf_usb_tx_complete, usb_buf);
	usb_buf->urb->transfer_flags = 0;
#ifdef CONFIG_USB_NO_TRANS_DMA_MAP
	usb_buf->urb->transfer_dma = usb_buf->data_dma_trans_addr;
	usb_buf->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
#endif
#ifdef CONFIG_USE_USB_ZERO_PACKET
	usb_buf->urb->transfer_flags |= URB_ZERO_PACKET;
#endif
	aicwf_usb_tx_queue(usb, &usb->tx_post_list, usb_buf,
			   &usb->tx_post_count, &usb->tx_post_lock);
#ifdef CONFIG_TX_TASKLET
	tasklet_schedule(&usb->xmit_tasklet);
#else
	complete(&bus->bustx_trgg);
#endif
	ret = 0;
	goto flow_ctrl;

recycle:
	kfree(usb_buf->tx_data);
	kfree(usb_buf->usb_align_data);
	usb_buf->tx_data = NULL;
	usb_buf->usb_align_data = NULL;
	usb_buf->cfm = false;
	aicwf_usb_tx_queue(usb, &usb->tx_free_list, usb_buf,
			   &usb->tx_free_count, &usb->tx_free_lock);
drop:
	if (sw->need_cfm)
		aicwf_usb_host_txdesc_take(&hw->usb_env,
					 sw->desc.host.status_desc_addr & 0x3fffffff);
	rwnx_usb_tx_drop(hw, skb);
flow_ctrl:
	spin_lock_irqsave(&usb->tx_flow_lock, flags);
	if (usb->tx_free_count < AICWF_USB_TX_LOW_WATER && !usb->tx_stopping) {
		usb->tbusy = true;
		aicwf_usb_tx_flowctrl(hw, true);
	}
	spin_unlock_irqrestore(&usb->tx_flow_lock, flags);
	return ret;
}
#endif
static int aicwf_usb_bus_start(struct device *dev)
{
	struct aicwf_bus *bus_if = dev_get_drvdata(dev);
	struct aic_usb_dev *usb_dev = bus_if->bus_priv.usb;
	int ret;

	if (READ_ONCE(usb_dev->state) == USB_UP_ST)
		return READ_ONCE(usb_dev->rx_prepare_ready) ? 0 : -EIO;

	/* Only probe starts this binding; data/commands wait for complete RX. */
	WRITE_ONCE(usb_dev->rx_prepare_ready, false);
	aicwf_usb_state_change(usb_dev, USB_STARTING_ST);
	ret = aicwf_usb_rx_prepare(usb_dev);
	if (ret)
		goto fail;
	if (usb_anchor_empty(&usb_dev->rx_submitted)) {
		ret = -EIO;
		goto fail;
	}
#ifdef CONFIG_USB_MSG_IN_EP
	if (usb_dev->msg_in_pipe) {
		ret = aicwf_usb_msg_rx_prepare(usb_dev);
		if (ret)
			goto fail;
		if (usb_anchor_empty(&usb_dev->msg_rx_submitted)) {
			ret = -EIO;
			goto fail;
		}
	}
#endif
	WRITE_ONCE(usb_dev->rx_prepare_ready, true);
	aicwf_usb_state_change(usb_dev, USB_UP_ST);
	return 0;
fail:
	/* A failed probe never resumes these consumers: fully drain this binding. */
	aicwf_usb_rx_quiesce(usb_dev);
	dev_err(dev, "RX preparation failed: %d\n", ret);
	return ret;
}

/* Idempotent final TX shutdown, while hw, vifs and the slab cache still exist. */
void aicwf_usb_tx_stop(struct aic_usb_dev *usb_dev)
{
	struct rwnx_hw *hw = usb_dev->rwnx_hw;
	struct aicwf_bus *bus = usb_dev->bus_if;
	struct aicwf_usb_buf *usb_buf;

	if (READ_ONCE(usb_dev->tx_stopping))
		return;
	if (hw)
		spin_lock_bh(&hw->tx_lock);
	WRITE_ONCE(usb_dev->tx_stopping, true);
	if (hw)
		spin_unlock_bh(&hw->tx_lock);
	if (bus->bustx_thread) {
		complete_all(&bus->bustx_trgg);
		kthread_stop(bus->bustx_thread);
		bus->bustx_thread = NULL;
	}
#ifdef CONFIG_TX_TASKLET
	tasklet_kill(&usb_dev->xmit_tasklet);
#endif
	usb_kill_anchored_urbs(&usb_dev->tx_submitted);
	/* Unsubmitted entries have no USB callback to release them. */
	while ((usb_buf = aicwf_usb_tx_dequeue(usb_dev, &usb_dev->tx_post_list,
					      &usb_dev->tx_post_count,
					      &usb_dev->tx_post_lock)))
		aicwf_usb_tx_finish(usb_buf, -ESHUTDOWN);
#ifndef CONFIG_USB_TX_AGGR
	flush_work(&usb_dev->tx_failed_work);
#endif
	if (hw && hw->usb_env.pthis) {
		spin_lock_bh(&hw->tx_lock);
		aicwf_usb_host_txdesc_flush(&hw->usb_env);
		spin_unlock_bh(&hw->tx_lock);
	}
}

static void aicwf_usb_cancel_all_urbs_(struct aic_usb_dev *usb_dev)
{
	aicwf_usb_tx_stop(usb_dev);
	if (usb_dev->msg_out_urb)
		usb_kill_urb(usb_dev->msg_out_urb);

    usb_kill_anchored_urbs(&usb_dev->rx_submitted);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->msg_in_pipe){
		usb_kill_anchored_urbs(&usb_dev->msg_rx_submitted);
	}
#endif
}

void aicwf_usb_cancel_all_urbs(struct aic_usb_dev *usb_dev){
	aicwf_usb_cancel_all_urbs_(usb_dev);
}


static void aicwf_usb_bus_stop(struct device *dev)
{
    struct aicwf_bus *bus_if = dev_get_drvdata(dev);
    struct aic_usb_dev *usb_dev = bus_if->bus_priv.usb;

	AICWFDBG(LOGINFO, "%s\r\n", __func__);
    if (usb_dev == NULL)
        return;

    if (usb_dev->state == USB_DOWN_ST)
        return;

    aicwf_usb_state_change(usb_dev, USB_DOWN_ST);
    //aicwf_usb_cancel_all_urbs(usb_dev);//AIDEN
}

/* Drain callbacks while keeping their private state available to the owner. */
void aicwf_usb_rx_quiesce(struct aic_usb_dev *usb)
{
	struct aicwf_bus *bus = usb->bus_if;

	aicwf_usb_state_change(usb, USB_DOWN_ST);
	/* state_change() may return early when USB was already down. */
	WRITE_ONCE(bus->state, BUS_DOWN_ST);
	cancel_work_sync(&usb->rx_urb_work);
#ifdef CONFIG_USB_MSG_IN_EP
	if (usb->msg_in_pipe)
		cancel_work_sync(&usb->msg_rx_urb_work);
#endif
	usb_kill_anchored_urbs(&usb->rx_submitted);
#ifdef CONFIG_USB_MSG_IN_EP
	if (usb->msg_in_pipe)
		usb_kill_anchored_urbs(&usb->msg_rx_submitted);
#endif
#ifdef CONFIG_RX_TASKLET
	if (usb->rx_tasklet_initialized) {
		tasklet_kill(&usb->recv_tasklet);
		usb->rx_tasklet_initialized = false;
	}
#endif
	if (bus->busrx_thread) {
		complete_all(&bus->busrx_trgg);
		kthread_stop(bus->busrx_thread);
		bus->busrx_thread = NULL;
	}
#ifdef CONFIG_USB_MSG_IN_EP
	if (bus->msg_busrx_thread) {
		complete_all(&bus->msg_busrx_trgg);
		kthread_stop(bus->msg_busrx_thread);
		bus->msg_busrx_thread = NULL;
	}
#endif
	/* An active RX consumer may have queued a refill during its drain. */
	cancel_work_sync(&usb->rx_urb_work);
#ifdef CONFIG_USB_MSG_IN_EP
	if (usb->msg_in_pipe)
		cancel_work_sync(&usb->msg_rx_urb_work);
#endif
}

/* Keep transport callbacks from outliving the wiphy and its private state. */
void aicwf_usb_rx_stop(struct aic_usb_dev *usb)
{
	aicwf_usb_rx_quiesce(usb);
	if (usb->rx_priv) {
		aicwf_rx_deinit(usb->rx_priv);
		usb->rx_priv = NULL;
	}
}

static void aicwf_usb_deinit(struct aic_usb_dev *usbdev)
{
    cancel_work_sync(&usbdev->rx_urb_work);
    aicwf_usb_free_urb(&usbdev->rx_free_list, &usbdev->rx_free_lock);
    aicwf_usb_free_urb(&usbdev->tx_free_list, &usbdev->tx_free_lock);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usbdev->msg_in_pipe){
		cancel_work_sync(&usbdev->msg_rx_urb_work);
		aicwf_usb_free_urb(&usbdev->msg_rx_free_list, &usbdev->msg_rx_free_lock);
	}
#endif

    usb_free_urb(usbdev->msg_out_urb);
}

static void aicwf_usb_rx_urb_work(struct work_struct *work)
{
    struct aic_usb_dev *usb_dev = container_of(work, struct aic_usb_dev, rx_urb_work);

	aicwf_usb_rx_submit_all_urb_(usb_dev);
}

#ifdef CONFIG_USB_MSG_IN_EP
static void aicwf_usb_msg_rx_urb_work(struct work_struct *work)
{
    struct aic_usb_dev *usb_dev = container_of(work, struct aic_usb_dev, msg_rx_urb_work);

	aicwf_usb_msg_rx_submit_all_urb_(usb_dev);
}
#endif

static int aicwf_usb_init(struct aic_usb_dev *usb_dev)
{
    int ret = 0;

    usb_dev->tbusy = false;
    usb_dev->state = USB_DOWN_ST;
	spin_lock_init(&usb_dev->rx_submit_lock);
	/* The allocation-error unwind also cancels these work items. */
	INIT_WORK(&usb_dev->rx_urb_work, aicwf_usb_rx_urb_work);
#ifdef CONFIG_USB_MSG_IN_EP
	if (usb_dev->msg_in_pipe)
		INIT_WORK(&usb_dev->msg_rx_urb_work, aicwf_usb_msg_rx_urb_work);
#endif

    init_waitqueue_head(&usb_dev->msg_wait);
    init_usb_anchor(&usb_dev->rx_submitted);
	init_usb_anchor(&usb_dev->tx_submitted);
#ifndef CONFIG_USB_TX_AGGR
	INIT_LIST_HEAD(&usb_dev->tx_failed_list);
	spin_lock_init(&usb_dev->tx_failed_lock);
	INIT_WORK(&usb_dev->tx_failed_work, aicwf_usb_tx_failed_work);
#endif
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->msg_in_pipe){
		init_usb_anchor(&usb_dev->msg_rx_submitted);
	}
#endif

    spin_lock_init(&usb_dev->tx_free_lock);
    spin_lock_init(&usb_dev->tx_post_lock);
    spin_lock_init(&usb_dev->rx_free_lock);
    spin_lock_init(&usb_dev->tx_flow_lock);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->msg_in_pipe){
		spin_lock_init(&usb_dev->msg_rx_free_lock);
	}
#endif

    INIT_LIST_HEAD(&usb_dev->rx_free_list);
    INIT_LIST_HEAD(&usb_dev->tx_free_list);
    INIT_LIST_HEAD(&usb_dev->tx_post_list);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->msg_in_pipe){
		INIT_LIST_HEAD(&usb_dev->msg_rx_free_list);
	}
#endif

	atomic_set(&rx_urb_cnt, 0);

    usb_dev->tx_free_count = 0;
    usb_dev->tx_post_count = 0;

    ret =  aicwf_usb_alloc_rx_urb(usb_dev);
    if (ret) {
        goto error;
    }
    ret =  aicwf_usb_alloc_tx_urb(usb_dev);
    if (ret) {
        goto error;
    }
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->msg_in_pipe){
		ret =  aicwf_usb_alloc_msg_rx_urb(usb_dev);
		if (ret) {
			goto error;
		}
	}
#endif


    usb_dev->msg_out_urb = usb_alloc_urb(0, GFP_ATOMIC);
    if (!usb_dev->msg_out_urb) {
        usb_err("usb_alloc_urb (msg out) failed\n");
        ret = ENOMEM;
        goto error;
    }

    return ret;
    error:
    usb_err("failed!\n");
    aicwf_usb_deinit(usb_dev);
    return ret;
}


static int aicwf_parse_usb(struct aic_usb_dev *usb_dev, struct usb_interface *interface)
{
    struct usb_interface_descriptor *interface_desc;
    struct usb_host_interface *host_interface;
    struct usb_endpoint_descriptor *endpoint;
    struct usb_device *usb = usb_dev->udev;
    int i, endpoints;
    u8 endpoint_num;
    int ret = 0;

    usb_dev->bulk_in_pipe = 0;
    usb_dev->bulk_out_pipe = 0;
#ifdef CONFIG_USB_MSG_OUT_EP
    usb_dev->msg_out_pipe = 0;
#endif
#ifdef CONFIG_USB_MSG_IN_EP
	usb_dev->msg_in_pipe = 0;
#endif

    host_interface = &interface->altsetting[0];
    interface_desc = &host_interface->desc;
    endpoints = interface_desc->bNumEndpoints;
	AICWFDBG(LOGINFO, "%s endpoints = %d\n", __func__, endpoints);

    /* Check device configuration */
    if (usb->descriptor.bNumConfigurations != 1) {
        usb_err("Number of configurations: %d not supported\n",
                        usb->descriptor.bNumConfigurations);
        ret = -ENODEV;
        goto exit;
    }

    /* Check deviceclass */
#ifndef CONFIG_USB_BT
    if (usb->descriptor.bDeviceClass != 0x00) {
        usb_err("DeviceClass %d not supported\n",
            usb->descriptor.bDeviceClass);
        ret = -ENODEV;
        goto exit;
    }
#endif

    /* Check interface number */
#ifdef CONFIG_USB_BT
    if (usb->actconfig->desc.bNumInterfaces != 1 && usb->actconfig->desc.bNumInterfaces != 3)
#else
    if (usb->actconfig->desc.bNumInterfaces != 1)
#endif
    {
	   AICWFDBG(LOGERROR, "Number of interfaces: %d not supported\n",
            usb->actconfig->desc.bNumInterfaces);
		if(usb_dev->chipid == PRODUCT_ID_AIC8800DC){
			AICWFDBG(LOGERROR, "AIC8800DC change to AIC8800DW\n");
			usb_dev->chipid = PRODUCT_ID_AIC8800DW;
		}else if (usb_dev->chipid == PRODUCT_ID_AIC8800DW) {
            AICWFDBG(LOGINFO, "AIC8800DW\n");
		} else if (usb_dev->chipid == PRODUCT_ID_AIC8800D80N) {
			AICWFDBG(LOGERROR, "AIC8800D80N change to AIC8800D80WN\n");
		} else if (usb_dev->chipid == PRODUCT_ID_AIC8800DLN) {
			AICWFDBG(LOGERROR, "AIC8800DLN change to AIC8800DWN\n");
		}else if(usb_dev->chipid == PRODUCT_ID_AIC8800D81X2 ||
                usb_dev->chipid == PRODUCT_ID_AIC8800D89X2){
            //TODO
        }else{
			ret = -ENODEV;
			goto exit;
		}
    }

    if ((interface_desc->bInterfaceClass != USB_CLASS_VENDOR_SPEC) ||
        (interface_desc->bInterfaceSubClass != 0xff) ||
        (interface_desc->bInterfaceProtocol != 0xff)) {
        usb_err("non WLAN interface %d: 0x%x:0x%x:0x%x\n",
            interface_desc->bInterfaceNumber, interface_desc->bInterfaceClass,
            interface_desc->bInterfaceSubClass, interface_desc->bInterfaceProtocol);
        ret = -ENODEV;
        goto exit;
    }

    for (i = 0; i < endpoints; i++) {
        endpoint = &host_interface->endpoint[i].desc;
        endpoint_num = usb_endpoint_num(endpoint);

        if (usb_endpoint_dir_in(endpoint) &&
            usb_endpoint_xfer_bulk(endpoint)) {
            if (!usb_dev->bulk_in_pipe) {
                usb_dev->bulk_in_pipe = usb_rcvbulkpipe(usb, endpoint_num);
            }
#ifdef CONFIG_USB_MSG_IN_EP
            else if (!usb_dev->msg_in_pipe) {
				if(usb_dev->chipid != PRODUCT_ID_AIC8801){
                	usb_dev->msg_in_pipe = usb_rcvbulkpipe(usb, endpoint_num);
				}
            }
#endif
        }

        if (usb_endpoint_dir_out(endpoint) &&
            usb_endpoint_xfer_bulk(endpoint)) {
            if (!usb_dev->bulk_out_pipe)
            {
                usb_dev->bulk_out_pipe = usb_sndbulkpipe(usb, endpoint_num);
            }
#ifdef CONFIG_USB_MSG_OUT_EP
             else if (!usb_dev->msg_out_pipe) {
                usb_dev->msg_out_pipe = usb_sndbulkpipe(usb, endpoint_num);
            }
#endif

        }
    }

    if (usb_dev->bulk_in_pipe == 0) {
        usb_err("No RX (in) Bulk EP found\n");
        ret = -ENODEV;
        goto exit;
    }
    if (usb_dev->bulk_out_pipe == 0) {
        usb_err("No TX (out) Bulk EP found\n");
        ret = -ENODEV;
        goto exit;
    }
#ifdef CONFIG_USB_MSG_OUT_EP
    if (usb_dev->msg_out_pipe == 0) {
        usb_err("No TX Msg (out) Bulk EP found\n");
    }
#endif
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->chipid != PRODUCT_ID_AIC8801){
		if (usb_dev->msg_in_pipe == 0) {
			AICWFDBG(LOGINFO, "No RX Msg (in) Bulk EP found\n");
		}
	}
#endif

    switch (usb->speed) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 6, 0)
    case USB_SPEED_SUPER_PLUS:
        AICWFDBG(LOGINFO, "Aic super plus speed USB device detected\n");
        break;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 0, 0)
    case USB_SPEED_SUPER:
        AICWFDBG(LOGINFO, "Aic super speed USB device detected\n");
        break;
#endif
    case USB_SPEED_HIGH:
        AICWFDBG(LOGINFO, "Aic high speed USB device detected\n");
        break;
    case USB_SPEED_FULL:
        AICWFDBG(LOGINFO, "Aic full speed USB device detected\n");
        break;
    default:
        AICWFDBG(LOGINFO, "Aic unknown speed(%d) USB device detected\n", usb->speed);
        break;
    }

    exit:
    return ret;
}



static struct aicwf_bus_ops aicwf_usb_bus_ops = {
    .start = aicwf_usb_bus_start,
    .stop = aicwf_usb_bus_stop,
    .txdata = aicwf_usb_bus_txdata,
    .txmsg = aicwf_usb_bus_txmsg,
};


#ifdef CONFIG_GPIO_WAKEUP

static irqreturn_t rwnx_irq_handler(int irq, void *para)
{
	unsigned long irqflags;
    spin_lock_irqsave(&irq_lock, irqflags);
    disable_irq_nosync(hostwake_irq_num);
    //do something
    printk("%s gpio irq trigger\r\n", __func__);
    spin_unlock_irqrestore(&irq_lock, irqflags);
    atomic_dec(&irq_count);
	return IRQ_HANDLED;
}


static int rwnx_register_hostwake_irq(struct device *dev)
{
	int ret = 0;
	uint irq_flags = 0;

	spin_lock_init(&irq_lock);

//Setting hostwake gpio for platform
//For Rockchip
#ifdef CONFIG_PLATFORM_ROCKCHIP
	hostwake_irq_num = rockchip_wifi_get_oob_irq();
	printk("%s hostwake_irq_num:%d \r\n", __func__, hostwake_irq_num);
	irq_flags = (IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHLEVEL | IORESOURCE_IRQ_SHAREABLE) & IRQF_TRIGGER_MASK;
	printk("%s irq_flags:%d \r\n", __func__, irq_flags);
	wakeup_enable = 1;
#endif //CONFIG_PLATFORM_ROCKCHIP

//For Allwinner
#ifdef CONFIG_PLATFORM_ALLWINNER
		int irq_flags;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
	hostwake_irq_num = sunxi_wlan_get_oob_irq(&irq_flags, &wakeup_enable);
#else
	hostwake_irq_num = sunxi_wlan_get_oob_irq();
	irq_flags = sunxi_wlan_get_oob_irq_flags();
	wakeup_enable = 1;
#endif
#endif //CONFIG_PLATFORM_ALLWINNER


	ret = request_irq(hostwake_irq_num,
				rwnx_irq_handler, IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND,
				"rwnx_irq_handler", NULL);

	enable_irq_wake(hostwake_irq_num);

	return ret;
}

static int rwnx_unregister_hostwake_irq(struct device *dev)
{
	wakeup_enable = 0;

	printk("%s hostwake_irq_num:%d \r\n", __func__, hostwake_irq_num);
	disable_irq_wake(hostwake_irq_num);
	free_irq(hostwake_irq_num, NULL);

	return 0;
}

#endif //CONFIG_GPIO_WAKEUP

static int aicwf_usb_chipmatch(struct aic_usb_dev *usb_dev, u16_l vid, u16_l pid){

	if(pid == USB_PRODUCT_ID_AIC8801){
		usb_dev->chipid = PRODUCT_ID_AIC8801;
		AICWFDBG(LOGINFO, "%s USE AIC8801\r\n", __func__);
		return 0;
	}else if(pid == USB_PRODUCT_ID_AIC8800DC || pid == USB_PRODUCT_ID_AIC8800DE
		|| pid == USB_PRODUCT_ID_TENDA
		|| pid == USB_PRODUCT_ID_TENDA_U2  || pid == USB_PRODUCT_ID_AIC8800FC_CUS1
		|| pid == USB_PRODUCT_ID_AIC8800FC_CUS2 || pid == USB_PRODUCT_ID_AIC8800FC_CUS3
		|| pid == USB_PRODUCT_ID_AIC8800FC_CUS4 || pid == USB_PRODUCT_ID_AIC8800FC_CUS5
		|| pid == USB_PRODUCT_ID_AIC8800FC_CUS6){
		usb_dev->chipid = PRODUCT_ID_AIC8800DC;
		AICWFDBG(LOGINFO, "%s USE AIC8800DC\r\n", __func__);
		return 0;
	}else if(pid == USB_PRODUCT_ID_AIC8800DW || pid == USB_PRODUCT_ID_TENDA_TX1U_NANO){
        usb_dev->chipid = PRODUCT_ID_AIC8800DW;
		AICWFDBG(LOGINFO, "%s USE AIC8800DW\r\n", __func__);
        return 0;
    }else if(pid == USB_PRODUCT_ID_AIC8800D81 || pid == USB_PRODUCT_ID_AIC8800D41
		|| pid == USB_PRODUCT_ID_TENDA_U11 || pid == USB_PRODUCT_ID_TENDA_U11_PRO
		|| pid == USB_PRODUCT_ID_AIC8800M80_CUS0 || pid == USB_PRODUCT_ID_AIC8800M80_CUS1
		|| pid == USB_PRODUCT_ID_AIC8800M80_CUS2
		|| pid == USB_PRODUCT_ID_AIC8800M80_CUS3 || pid == USB_PRODUCT_ID_AIC8800M80_CUS4
		|| pid == USB_PRODUCT_ID_AIC8800M80_CUS5 || pid == USB_PRODUCT_ID_AIC8800M80_CUS6
		|| pid == USB_PRODUCT_ID_AIC8800M80_CUS7 || pid == USB_PRODUCT_ID_AIC8800M80_CUS8
        || pid == USB_PRODUCT_ID_AIC8800D80_UGREEN || pid == USB_PRODUCT_ID_MERCURY
        || pid == USB_PRODUCT_ID_TP){
        usb_dev->chipid = PRODUCT_ID_AIC8800D81;
        aicwf_usb_rx_aggr = true;
        AICWFDBG(LOGINFO, "%s USE AIC8800D81\r\n", __func__);
        return 0;
    }else if(pid == USB_PRODUCT_ID_AIC8800D81X2){
        usb_dev->chipid = PRODUCT_ID_AIC8800D81X2;
        aicwf_usb_max_pkt_size = AICWF_USB_MAX_AMSDU_PKT_SIZE;
        if (usb_dev->udev->speed <= USB_SPEED_HIGH) {
            aicwf_usb_rx_aggr = true;
        } else {
            #ifdef CONFIG_PLATFORM_HI
            aicwf_usb_rx_aggr = true;
            #else
            aicwf_usb_rx_aggr = false;
            #endif
        }
        AICWFDBG(LOGINFO, "%s USE AIC8800D81X2\r\n", __func__);
        return 0;
    }else if(pid == USB_PRODUCT_ID_AIC8800D89X2){
        usb_dev->chipid = PRODUCT_ID_AIC8800D89X2;
        aicwf_usb_max_pkt_size = AICWF_USB_MAX_AMSDU_PKT_SIZE;
        if (usb_dev->udev->speed <= USB_SPEED_HIGH) {
            aicwf_usb_rx_aggr = true;
        } else {
            #ifdef CONFIG_PLATFORM_HI
            aicwf_usb_rx_aggr = true;
            #else
            aicwf_usb_rx_aggr = false;
            #endif
        }
        AICWFDBG(LOGINFO, "%s USE AIC8800D89X2\r\n", __func__);
        return 0;
    } else if ((pid == USB_PRODUCT_ID_AIC8800D80N) ||
               (pid == USB_PRODUCT_ID_AIC8800D80LN) ||
               (pid == USB_PRODUCT_ID_AIC8800D80WN) ||
               (pid == USB_PRODUCT_ID_AIC8800D40N) ||
               (pid == USB_PRODUCT_ID_AIC8800D40LN) ||
               (pid == USB_PRODUCT_ID_AIC8800D40WN)) {
        usb_dev->chipid = PRODUCT_ID_AIC8800D80N;
        aicwf_usb_rx_aggr = true;
        AICWFDBG(LOGINFO, "%s USE AIC8800D80N\r\n", __func__);
        return 0;
    } else if ((pid == USB_PRODUCT_ID_AIC8800DLN) ||
               (pid == USB_PRODUCT_ID_AIC8800DWN)) {
        usb_dev->chipid = PRODUCT_ID_AIC8800DLN;
        aicwf_usb_rx_aggr = true;
        AICWFDBG(LOGINFO, "%s USE AIC8800DLN\r\n", __func__);
        return 0;
    } else{
		return -1;
	}
}


static int aicwf_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct aicwf_bus *bus = NULL;
	struct aic_usb_dev *usb;
	bool bus_initialized = false;
	int ret;

	usb = kzalloc(sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;
	usb->udev = udev;
	usb->dev = &udev->dev;
	aicwf_usb_recovery_init(usb);
	usb_set_intfdata(intf, usb);
	usb->usb_tx_buf = vzalloc(sizeof(*usb->usb_tx_buf) * AICWF_USB_TX_URBS);
	usb->usb_rx_buf = vzalloc(sizeof(*usb->usb_rx_buf) * AICWF_USB_RX_URBS);
	if (!usb->usb_tx_buf || !usb->usb_rx_buf) {
		ret = -ENOMEM;
		goto out_free;
	}
	ret = aicwf_usb_chipmatch(usb, id->idVendor, id->idProduct);
	if (ret) {
		ret = -ENODEV;
		goto out_free;
	}
	ret = aicwf_parse_usb(usb, intf);
	if (ret)
		goto out_free;
	ret = aicwf_usb_init(usb);
	if (ret) {
		/* usb_init releases its own partial URBs. */
		if (ret > 0)
			ret = -ret;
		goto out_free;
	}
	bus = kzalloc(sizeof(*bus), GFP_KERNEL);
	if (!bus) {
		ret = -ENOMEM;
		goto out_free_usb;
	}
	bus->dev = usb->dev;
	bus->bus_priv.usb = usb;
	bus->ops = &aicwf_usb_bus_ops;
	usb->bus_if = bus;
	dev_set_drvdata(usb->dev, bus);
	usb->rx_priv = aicwf_rx_init(usb);
	if (!usb->rx_priv) {
		ret = -ENOMEM;
		goto out_free_bus;
	}
#ifdef CONFIG_USB_TX_AGGR
	usb->tx_priv = aicwf_tx_init(usb);
	if (!usb->tx_priv) {
		ret = -ENOMEM;
		goto out_free_bus;
	}
	aicwf_frame_queue_init(&usb->tx_priv->txq, 8, TXQLEN);
	spin_lock_init(&usb->tx_priv->txqlock);
	spin_lock_init(&usb->tx_priv->txdlock);
#endif
	bus_initialized = true;
	ret = aicwf_bus_init(0, usb->dev);
	if (ret)
		goto out_free_bus;
	ret = aicwf_bus_start(bus);
	if (ret)
		goto out_free_bus;
	ret = aicwf_rwnx_usb_platform_init(usb);
	if (ret)
		goto out_free_bus;
	aicwf_hostif_ready();
#ifdef CONFIG_GPIO_WAKEUP
	rwnx_register_hostwake_irq(usb->dev);
#endif
	aicwf_usb_recovery_start(usb);
	return 0;

out_free_bus:
	aicwf_usb_recovery_stop(usb);
	if (bus_initialized)
		aicwf_bus_deinit(usb->dev);
	else if (usb->rx_priv) {
		/* No bus thread or URB has been started yet. */
		aicwf_rx_deinit(usb->rx_priv);
		usb->rx_priv = NULL;
	}
#ifdef CONFIG_USB_TX_AGGR
	if (usb->tx_priv) {
		aicwf_tx_deinit(usb->tx_priv);
		usb->tx_priv = NULL;
	}
#endif
	dev_set_drvdata(usb->dev, NULL);
	usb->bus_if = NULL;
	kfree(bus);
out_free_usb:
	aicwf_usb_deinit(usb);
out_free:
	aicwf_usb_recovery_stop(usb);
	usb_set_intfdata(intf, NULL);
	vfree(usb->usb_tx_buf);
	vfree(usb->usb_rx_buf);
	kfree(usb);
	return ret;
}

static void aicwf_usb_disconnect(struct usb_interface *intf)
{
	struct aic_usb_dev *usb = usb_get_intfdata(intf);

	if (!usb)
		return;
	/* Join recovery before taking any teardown lock it could wait behind. */
	aicwf_usb_recovery_stop(usb);
	usb_set_intfdata(intf, NULL);
	down(&aicwf_deinit_sem);
	atomic_set(&aicwf_deinit_atomic, 0);
#ifdef CONFIG_GPIO_WAKEUP
	rwnx_unregister_hostwake_irq(usb->dev);
#endif
	aicwf_bus_deinit(usb->dev);
	aicwf_usb_deinit(usb);
#ifdef CONFIG_USB_TX_AGGR
	if (usb->tx_priv)
		aicwf_tx_deinit(usb->tx_priv);
#endif
	dev_set_drvdata(usb->dev, NULL);
	kfree(usb->bus_if);
	usb->bus_if = NULL;
	vfree(usb->usb_tx_buf);
	vfree(usb->usb_rx_buf);
	kfree(usb);
	atomic_set(&aicwf_deinit_atomic, 1);
	up(&aicwf_deinit_sem);
}

static struct usb_device_id aicwf_usb_id_table[] = {
#ifndef CONFIG_USB_BT
    {USB_DEVICE(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8800)},
#else
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8801, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8800D81, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8800D41, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8800DC, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8800DE, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_TENDA_V2, USB_PRODUCT_ID_TENDA_TX1U_NANO, 0xff, 0xff, 0xff)},
    {USB_DEVICE(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8800DW)},
    {USB_DEVICE(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8800M80_CUS1)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800D81X2, 0xff, 0xff, 0xff)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800D89X2)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800D80N, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800D80LN, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800D80WN, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800D40N, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800D40LN, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800D40WN, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800DLN, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800DWN, 0xff, 0xff, 0xff)},
    {USB_DEVICE(USB_VENDOR_ID_TENDA, USB_PRODUCT_ID_TENDA)},
    {USB_DEVICE(USB_VENDOR_ID_TENDA, USB_PRODUCT_ID_TENDA_U2)},
    {USB_DEVICE(USB_VENDOR_ID_TENDA, USB_PRODUCT_ID_TENDA_U11)},
    {USB_DEVICE(USB_VENDOR_ID_TENDA, USB_PRODUCT_ID_TENDA_U11_PRO)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800D81)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800FC_CUS1)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800FC_CUS2)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800FC_CUS3)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800FC_CUS4)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800FC_CUS5)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800FC_CUS6)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800M80_CUS0)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800M80_CUS1)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800M80_CUS2)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800M80_CUS3)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800M80_CUS4)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800M80_CUS5)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800M80_CUS6)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800M80_CUS7)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800M80_CUS8)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800D80_UGREEN)},
    {USB_DEVICE(USB_VENDOR_ID_TP, USB_PRODUCT_ID_MERCURY)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_TP, USB_PRODUCT_ID_TP, 0xff, 0xff, 0xff)},
#endif
    {}
};

MODULE_DEVICE_TABLE(usb, aicwf_usb_id_table);

static struct usb_driver aicwf_usbdrvr = {
    .name = KBUILD_MODNAME,
    .probe = aicwf_usb_probe,
    .disconnect = aicwf_usb_disconnect,
    .id_table = aicwf_usb_id_table,
	/* Keep endpoints usable for acknowledged stack-stop; disconnect drains them. */
	.soft_unbind = 1,
	/*
	 * Firmware state retention and wake-on-Wi-Fi are not implemented. USB core
	 * unbinds before system sleep and reprobes after resume/restore, giving
	 * command, RX/TX and station state a fresh lifetime even after a reset.
	 */
	.supports_autosuspend = 0,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
    .disable_hub_initiated_lpm = 1,
#endif
};

void aicwf_usb_register(void)
{
    if (usb_register(&aicwf_usbdrvr) < 0) {
        usb_err("usb_register failed\n");
    }
}

void aicwf_usb_exit(void)
{
	/* USB core drains each binding through disconnect exactly once. */
	usb_deregister(&aicwf_usbdrvr);
}
