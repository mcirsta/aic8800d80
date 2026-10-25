/**
 * aicwf_bus.c
 *
 * bus function declarations
 *
 * Copyright (C) AICSemi 2018-2020
 */

#include <linux/kthread.h>
#include <linux/netdevice.h>
#include <linux/printk.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/completion.h>
#include <linux/semaphore.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <linux/vmalloc.h>
#include <linux/unaligned.h>
#include "lmac_msg.h"
#include "aicwf_txrxif.h"
#include "rwnx_platform.h"
#include "rwnx_defs.h"
#include "rwnx_msg_rx.h"
#include "rwnx_rx.h"
#include "aicwf_rx_prealloc.h"
#ifdef AICWF_SDIO_SUPPORT
#include "sdio_host.h"
#endif

#ifdef CONFIG_RX_TASKLET
int aicwf_tasklet_rxframes(struct aicwf_rx_priv *rx_priv);
#endif
#ifdef CONFIG_TX_TASKLET
void aicwf_tasklet_tx_process(struct aic_usb_dev *usb_dev);
#endif
extern bool aicwf_usb_rx_aggr;

#ifdef CONFIG_PREALLOC_RX_SKB
void aicwf_rxframe_queue_init_2(struct rx_frame_queue *pq, int max_len)
{
    //int prio;

    memset(pq, 0, offsetof(struct rx_frame_queue, queuelist) + (sizeof(struct list_head)));
    pq->qmax = (u16)max_len;
    INIT_LIST_HEAD(&pq->queuelist);
#if 0
    memset(pq, 0, offsetof(struct rx_frame_queue, queuelist) + (sizeof(struct list_head) * num_prio));
    pq->num_prio = (u16)num_prio;
    pq->qmax = (u16)max_len;

    for (prio = 0; prio < num_prio; prio++) {
        INIT_LIST_HEAD(&pq->queuelist[prio]);
    }
#endif
}

//extern struct aic_sdio_dev *g_sdiodev;
void rxbuff_queue_flush(struct aicwf_rx_priv* rx_priv)
{

    //int prio;
	struct rx_frame_queue *pq = &rx_priv->rxq;
    struct list_head *pos;
    struct list_head *n;
    struct list_head *head;
    struct rx_buff *tempbuf = NULL;

    head = &pq->queuelist;
    list_for_each_safe(pos, n, head) {
        tempbuf = list_entry(pos, struct rx_buff, queue);
        list_del_init(&tempbuf->queue);
#if 0
        rxbuff_free(tempbuf);
#else
        aicwf_prealloc_rxbuff_free(tempbuf, &rx_priv->rxbuff_lock);
#endif
        pq->qcnt--;
    }
}
#endif

int aicwf_bus_init(uint bus_hdrlen, struct device *dev)
{
	struct aicwf_bus *bus_if;
	int ret;

	if (!dev)
		return -ENODEV;
	bus_if = dev_get_drvdata(dev);
	if (!bus_if)
		return -ENODEV;
	/* Every partial unwind may wake/join these consumers. */
	init_completion(&bus_if->bustx_trgg);
	init_completion(&bus_if->busrx_trgg);
#ifdef CONFIG_USB_MSG_IN_EP
	if (bus_if->bus_priv.usb->msg_in_pipe)
		init_completion(&bus_if->msg_busrx_trgg);
#endif
#ifdef AICWF_USB_SUPPORT
#ifdef CONFIG_RX_TASKLET
	tasklet_init(&bus_if->bus_priv.usb->recv_tasklet,
		(void (*)(unsigned long))aicwf_tasklet_rxframes,
		(unsigned long)bus_if->bus_priv.usb->rx_priv);
	bus_if->bus_priv.usb->rx_tasklet_initialized = true;
#endif
#ifdef CONFIG_TX_TASKLET
	tasklet_init(&bus_if->bus_priv.usb->xmit_tasklet,
		(void (*)(unsigned long))aicwf_tasklet_tx_process,
		(unsigned long)bus_if->bus_priv.usb);
#endif
#endif
#if defined(CONFIG_USB_SUPPORT) && defined(CONFIG_USB_NO_TRANS_DMA_MAP)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
	bus_if->cmd_buf = usb_alloc_coherent(bus_if->bus_priv.usb->udev,
		CMD_BUF_MAX, GFP_KERNEL,
		&bus_if->bus_priv.usb->cmd_dma_trans_addr);
#else
	bus_if->cmd_buf = usb_buffer_alloc(bus_if->bus_priv.usb->udev,
		CMD_BUF_MAX, GFP_KERNEL,
		&bus_if->bus_priv.usb->cmd_dma_trans_addr);
#endif
#else
	bus_if->cmd_buf = kzalloc(CMD_BUF_MAX, GFP_KERNEL);
#endif
	if (!bus_if->cmd_buf) {
		ret = -ENOMEM;
		goto fail;
	}
	memset(bus_if->cmd_buf, 0, CMD_BUF_MAX);
#ifdef AICWF_SDIO_SUPPORT
	bus_if->bustx_thread = kthread_run(sdio_bustx_thread, bus_if,
					"aicwf_bustx_thread");
	if (IS_ERR(bus_if->bustx_thread)) {
		ret = PTR_ERR(bus_if->bustx_thread);
		bus_if->bustx_thread = NULL;
		goto fail;
	}
	bus_if->busrx_thread = kthread_run(sdio_busrx_thread,
		bus_if->bus_priv.sdio->rx_priv, "aicwf_busrx_thread");
#endif
#ifdef AICWF_USB_SUPPORT
	bus_if->busrx_thread = kthread_run(usb_busrx_thread,
		bus_if->bus_priv.usb->rx_priv, "aicwf_busrx_thread");
#endif
	if (IS_ERR(bus_if->busrx_thread)) {
		ret = PTR_ERR(bus_if->busrx_thread);
		bus_if->busrx_thread = NULL;
		goto fail;
	}
#ifdef AICWF_USB_SUPPORT
	bus_if->bustx_thread = kthread_run(usb_bustx_thread, bus_if,
					"aicwf_bustx_thread");
	if (IS_ERR(bus_if->bustx_thread)) {
		ret = PTR_ERR(bus_if->bustx_thread);
		bus_if->bustx_thread = NULL;
		goto fail;
	}
#ifdef CONFIG_USB_MSG_IN_EP
	if (bus_if->bus_priv.usb->msg_in_pipe) {
		bus_if->msg_busrx_thread = kthread_run(usb_msg_busrx_thread,
			bus_if->bus_priv.usb->rx_priv, "aicwf_msg_busrx_thread");
		if (IS_ERR(bus_if->msg_busrx_thread)) {
			ret = PTR_ERR(bus_if->msg_busrx_thread);
			bus_if->msg_busrx_thread = NULL;
			goto fail;
		}
	}
#endif
#endif
	return 0;
fail:
	/* Both USB probe and SDIO release own the single partial unwind. */
	return ret;
}

void aicwf_usb_cancel_all_urbs(struct aic_usb_dev *usb_dev);

void aicwf_bus_deinit(struct device *dev)
{
    struct aicwf_bus *bus_if;
#ifdef AICWF_USB_SUPPORT
    struct aic_usb_dev *usb;
#endif
#ifdef AICWF_SDIO_SUPPORT
    struct aic_sdio_dev *sdiodev;
#endif

    if (!dev) {
        txrx_err("device not found\n");
        return;
    }
	AICWFDBG(LOGINFO, "%s Enter\r\n", __func__);

    bus_if = dev_get_drvdata(dev);
	if (!bus_if)
		return;
    aicwf_bus_stop(bus_if);

#ifdef AICWF_USB_SUPPORT
    usb = bus_if->bus_priv.usb;
	aicwf_usb_cancel_all_urbs(usb);//AIDEN

	aicwf_rwnx_usb_platform_deinit(usb);

#endif
#ifdef AICWF_SDIO_SUPPORT
    sdiodev =bus_if->bus_priv.sdio;
    if(g_rwnx_plat && g_rwnx_plat->enabled)
        rwnx_platform_deinit(sdiodev->rwnx_hw);
#endif

    if (bus_if->cmd_buf) {
        #if defined CONFIG_USB_SUPPORT && defined CONFIG_USB_NO_TRANS_DMA_MAP
        #if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35))
        usb_free_coherent(bus_if->bus_priv.usb->udev, CMD_BUF_MAX, bus_if->cmd_buf, bus_if->bus_priv.usb->cmd_dma_trans_addr);
        #else
        usb_buffer_free(bus_if->bus_priv.usb->udev, CMD_BUF_MAX, bus_if->cmd_buf, bus_if->bus_priv.usb->cmd_dma_trans_addr);
        #endif
        #else
        kfree(bus_if->cmd_buf);
        #endif
        bus_if->cmd_buf = NULL;
    }

    if (bus_if->bustx_thread) {
		AICWFDBG(LOGINFO, "%s stop to bustx_thread!! \r\n", __func__);


		complete_all(&bus_if->bustx_trgg);
        kthread_stop(bus_if->bustx_thread);
        bus_if->bustx_thread = NULL;
    }

	aicwf_usb_rx_stop(usb);

    rwnx_cmd_mgr_deinit(&usb->cmd_mgr);

#ifdef CONFIG_TX_TASKLET//AIDEN tasklet
	tasklet_kill(&usb->xmit_tasklet);
#endif

	AICWFDBG(LOGINFO, "%s Exit \n", __func__);
}

void aicwf_frame_tx(void *dev, struct sk_buff *skb)
{
#ifdef AICWF_SDIO_SUPPORT
    struct aic_sdio_dev *sdiodev = (struct aic_sdio_dev *)dev;
    aicwf_bus_txdata(sdiodev->bus_if, skb);
#else
    struct aic_usb_dev *usbdev = (struct aic_usb_dev *)dev;

    aicwf_bus_txdata(usbdev->bus_if, skb);
#endif
}

struct aicwf_tx_priv* aicwf_tx_init(void *arg)
{
    struct aicwf_tx_priv* tx_priv;

    tx_priv = kzalloc(sizeof(struct aicwf_tx_priv), GFP_KERNEL);
    if (!tx_priv)
        return NULL;

#ifdef AICWF_SDIO_SUPPORT
    tx_priv->sdiodev = (struct aic_sdio_dev *)arg;
#else
    tx_priv->usbdev = (struct aic_usb_dev *)arg;
#endif

    atomic_set(&tx_priv->aggr_count, 0);
    tx_priv->aggr_buf = dev_alloc_skb(MAX_AGGR_TXPKT_LEN);
    if(!tx_priv->aggr_buf) {
        txrx_err("Alloc bus->txdata_buf failed!\n");
        kfree(tx_priv);
        return NULL;
    }
    tx_priv->head = tx_priv->aggr_buf->data;
    tx_priv->tail = tx_priv->aggr_buf->data;

    return tx_priv;
}

void aicwf_tx_deinit(struct aicwf_tx_priv* tx_priv)
{
    if (tx_priv && tx_priv->aggr_buf)
        dev_kfree_skb(tx_priv->aggr_buf);

    kfree(tx_priv);
}

#ifdef AICWF_USB_SUPPORT
extern bool rx_urb_sched;

struct aicwf_rx_record {
	size_t length;
	size_t step;
	size_t payload_len;
	u8 type;
	bool config;
};

/* One transfer may contain a sequence of complete records and zero padding. */
static int aicwf_rx_record_decode(const u8 *data, size_t available,
				  bool aggregate, struct aicwf_rx_record *record)
{
	size_t payload_len, length, step;

	if (aggregate && !memchr_inv(data, 0, available))
		return 1;
	if (available < 4)
		return -EMSGSIZE;

	payload_len = get_unaligned_le16(data);
	record->config = !!(data[2] & USB_TYPE_CFG);
	record->type = data[2] & 0x7f;
	if (record->config) {
		length = 4 + payload_len;
		step = 4 + ALIGN(payload_len, RX_ALIGNMENT);
	} else {
		length = RX_HWHRD_LEN + payload_len;
		step = length;
	}
	if (!aggregate)
		step = length;
	if (!payload_len || length > available || step > available)
		return -EMSGSIZE;
	record->length = length;
	record->step = step;
	record->payload_len = payload_len;
	return 0;
}

/* Input storage is borrowed; each allocated data skb transfers to RX once. */
static int aicwf_process_rx_buffer(struct aicwf_rx_priv *rx_priv,
				   const u8 *data, size_t len, gfp_t flags,
				   bool aggregate)
{
	/* Pairs with the initialized owner publication in cfg80211 init. */
	struct rwnx_hw *hw = smp_load_acquire(&rx_priv->usbdev->rwnx_hw);
	struct aicwf_rx_record record;
	struct sk_buff *skb;
	u32 cfm[2];
	void *msg;
	int ret;

	if (!hw)
		return -ENODEV;
	while (len) {
		ret = aicwf_rx_record_decode(data, len, aggregate, &record);
		if (ret)
			return ret == 1 ? 0 : ret;
		if (!record.config) {
			skb = __dev_alloc_skb(record.length + CCMP_OR_WEP_INFO,
					      flags);
			if (!skb)
				return -ENOMEM;
			skb_put_data(skb, data, record.length);
			rwnx_rxdataind_aicwf(hw, skb, rx_priv);
		} else {
			switch (record.type) {
			case USB_TYPE_CFG_CMD_RSP:
				if (rx_priv->usbdev->bus_if->state == BUS_DOWN_ST)
					break;
				msg = kmemdup(data + 4, record.payload_len, flags);
				if (!msg)
					return -ENOMEM;
				ret = rwnx_rx_handle_msg(hw, msg, record.payload_len);
				kfree(msg);
				if (ret)
					return ret;
				break;
			case USB_TYPE_CFG_DATA_CFM:
				if (record.payload_len < sizeof(cfm))
					return -EMSGSIZE;
				cfm[0] = get_unaligned_le32(data + 4);
				cfm[1] = get_unaligned_le32(data + 8);
				aicwf_usb_host_tx_cfm_handler(&hw->usb_env, cfm);
				break;
			case USB_TYPE_CFG_PRINT:
				msg = kmemdup(data + 4, record.payload_len, flags);
				if (!msg)
					return -ENOMEM;
				rwnx_rx_handle_print(hw, msg, record.payload_len);
				kfree(msg);
				break;
			default:
				return -EPROTO;
			}
		}
		if (!aggregate)
			break;
		data += record.step;
		len -= record.step;
	}
	return 0;
}

#ifndef CONFIG_PREALLOC_RX_SKB
/* Return one only when the data consumer owns the original skb. */
static int aicwf_process_rx_skb(struct aicwf_rx_priv *rx_priv,
				struct sk_buff *skb, gfp_t flags, bool aggregate)
{
	struct aicwf_rx_record record;
	int ret;

	if (skb_linearize(skb))
		return -ENOMEM;
	if (!aggregate) {
		ret = aicwf_rx_record_decode(skb->data, skb->len, false, &record);
		if (ret)
			return ret;
		if (!record.config) {
			struct rwnx_hw *hw;

			/* Observe initialized fields before using the published owner. */
			hw = smp_load_acquire(&rx_priv->usbdev->rwnx_hw);
			if (!hw)
				return -ENODEV;
			skb_trim(skb, record.length);
			rwnx_rxdataind_aicwf(hw, skb, rx_priv);
			return 1;
		}
	}
	return aicwf_process_rx_buffer(rx_priv, skb->data, skb->len, flags,
				      aggregate);
}
#endif

static int aicwf_drain_rx_frames(struct aicwf_rx_priv *rx_priv, gfp_t flags)
{
	unsigned long irqflags;
	int ret, first_error = 0;
#ifdef CONFIG_PREALLOC_RX_SKB
	struct rx_buff *buffer;
#else
	struct sk_buff *skb;
#endif

	for (;;) {
		spin_lock_irqsave(&rx_priv->rxqlock, irqflags);
#ifdef CONFIG_PREALLOC_RX_SKB
		if (!rx_priv->rxq.qcnt) {
			spin_unlock_irqrestore(&rx_priv->rxqlock, irqflags);
			break;
		}
		buffer = rxbuff_dequeue(&rx_priv->rxq);
#else
		skb = aicwf_frame_dequeue(&rx_priv->rxq);
#endif
		spin_unlock_irqrestore(&rx_priv->rxqlock, irqflags);
#ifdef CONFIG_PREALLOC_RX_SKB
		if (!buffer)
			break;
		ret = aicwf_process_rx_buffer(rx_priv, buffer->data, buffer->len,
					     flags, aicwf_usb_rx_aggr);
		aicwf_prealloc_rxbuff_free(buffer, &rx_priv->rxbuff_lock);
#else
		if (!skb)
			break;
		ret = aicwf_process_rx_skb(rx_priv, skb, flags, aicwf_usb_rx_aggr);
		if (ret != 1)
			dev_kfree_skb_any(skb);
#endif
		atomic_dec(&rx_priv->rx_cnt);
		if (ret < 0 && !first_error)
			first_error = ret;
		if (rx_urb_sched) {
			schedule_work(&rx_priv->usbdev->rx_urb_work);
			rx_urb_sched = false;
		}
	}
	return first_error;
}

#ifdef CONFIG_RX_TASKLET
int aicwf_tasklet_rxframes(struct aicwf_rx_priv *rx_priv)
{
	return aicwf_drain_rx_frames(rx_priv, GFP_ATOMIC);
}
#endif
#endif /* AICWF_USB_SUPPORT */

int aicwf_process_rxframes(struct aicwf_rx_priv *rx_priv)
{
#ifdef AICWF_SDIO_SUPPORT
	int ret = 0;
	unsigned long flags = 0;
	struct sk_buff *skb = NULL;
	u16 pkt_len = 0;
	struct sk_buff *skb_inblock = NULL;
	size_t aggr_len = 0, adjust_len = 0;
	u8 *data = NULL;
	u8_l *msg = NULL;

	while (1) {
		spin_lock_irqsave(&rx_priv->rxqlock, flags);
		if (aicwf_is_framequeue_empty(&rx_priv->rxq)) {
			spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
			break;
		}
		skb = aicwf_frame_dequeue(&rx_priv->rxq);
		spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
		if (skb == NULL) {
			txrx_err("skb_error\r\n");
			break;
		}
		while (skb->len) {
			data = skb->data;
			if (!memchr_inv(data, 0, skb->len))
				break;
			if (skb->len < 4)
				break;
			pkt_len = get_unaligned_le16(data);
			if (!pkt_len)
				break;

			if ((skb->data[2] & SDIO_TYPE_CFG) != SDIO_TYPE_CFG) {
				if (skb->len < RX_HWHRD_LEN ||
				    pkt_len > skb->len - RX_HWHRD_LEN)
					break;
				aggr_len = pkt_len + RX_HWHRD_LEN;

				if (aggr_len & (RX_ALIGNMENT - 1))
					adjust_len = roundup(aggr_len, RX_ALIGNMENT);
				else
					adjust_len = aggr_len;

				if (adjust_len > skb->len)
					break;
				skb_inblock = __dev_alloc_skb(aggr_len + CCMP_OR_WEP_INFO,
							      GFP_KERNEL);
				if (!skb_inblock) {
					skb_pull(skb, adjust_len);
					continue;
				}

				skb_put(skb_inblock, aggr_len);
				memcpy(skb_inblock->data, data, aggr_len);
				rwnx_rxdataind_aicwf(rx_priv->sdiodev->rwnx_hw,
						     skb_inblock, rx_priv);
				skb_pull(skb, adjust_len);
			} else {
				aggr_len = pkt_len;

				if (aggr_len & (RX_ALIGNMENT - 1))
					adjust_len = roundup(aggr_len, RX_ALIGNMENT);
				else
					adjust_len = aggr_len;

				if (aggr_len > skb->len - 4 || adjust_len > skb->len - 4)
					break;
				msg = kmalloc(aggr_len + 4, GFP_KERNEL);
				if (!msg) {
					txrx_err("no more space for msg!\n");
					aicwf_dev_skb_free(skb);
					return -EBADE;
				}

				memcpy(msg, data, aggr_len + 4);
				if ((*(msg + 2) & 0x7f) == SDIO_TYPE_CFG_CMD_RSP)
					rwnx_rx_handle_msg(rx_priv->sdiodev->rwnx_hw,
							   (struct ipc_e2a_msg *)(msg + 4),
							   aggr_len);

				if ((*(msg + 2) & 0x7f) == SDIO_TYPE_CFG_DATA_CFM && aggr_len >= 8)
					aicwf_sdio_host_tx_cfm_handler(
						&rx_priv->sdiodev->rwnx_hw->sdio_env,
						(u32 *)(msg + 4));
				skb_pull(skb, adjust_len + 4);
				kfree(msg);
			}
		}

		dev_kfree_skb(skb);
		atomic_dec(&rx_priv->rx_cnt);
	}

	aicwf_sdio_pwr_stctl(rx_priv->sdiodev, SDIO_ACTIVE_ST);

	return ret;
#else /* AICWF_USB_SUPPORT */
	return aicwf_drain_rx_frames(rx_priv, GFP_KERNEL);
#endif //AICWF_SDIO_SUPPORT
}


#ifdef CONFIG_USB_MSG_IN_EP
int aicwf_process_msg_rxframes(struct aicwf_rx_priv *rx_priv)
{
	struct sk_buff *skb;
	unsigned long flags;
	int ret, first_error = 0;

	for (;;) {
		spin_lock_irqsave(&rx_priv->msg_rxqlock, flags);
		skb = aicwf_frame_dequeue(&rx_priv->msg_rxq);
		spin_unlock_irqrestore(&rx_priv->msg_rxqlock, flags);
		if (!skb)
			break;
		ret = skb_linearize(skb);
		if (!ret)
			ret = aicwf_process_rx_buffer(rx_priv, skb->data, skb->len,
						     GFP_KERNEL, false);
		dev_kfree_skb_any(skb);
		atomic_dec(&rx_priv->msg_rx_cnt);
		if (ret < 0 && !first_error)
			first_error = ret;
	}
	return first_error;
}
#endif

#ifdef AICWF_RX_REORDER
static struct recv_msdu *aicwf_rxframe_queue_init(struct list_head *q, int qsize)
{
    int i;
    struct recv_msdu *req, *reqs;

    reqs = vmalloc(qsize*sizeof(struct recv_msdu));
    if (reqs == NULL)
        return NULL;

    req = reqs;
    for (i = 0; i < qsize; i++) {
        INIT_LIST_HEAD(&req->rxframe_list);
        list_add(&req->rxframe_list, q);
        //req->len = 0;
        req++;
    }

    return reqs;
}
#endif
struct aicwf_rx_priv *aicwf_rx_init(void *arg)
{
    struct aicwf_rx_priv* rx_priv;
    rx_priv = kzalloc(sizeof(struct aicwf_rx_priv), GFP_KERNEL);
    if (!rx_priv)
        return NULL;

#ifdef AICWF_SDIO_SUPPORT
    rx_priv->sdiodev = (struct aic_sdio_dev *)arg;
#else
    rx_priv->usbdev = (struct aic_usb_dev *)arg;
#endif

#ifdef CONFIG_PREALLOC_RX_SKB
	aicwf_rxframe_queue_init_2(&rx_priv->rxq, MAX_RXQLEN);
#else
    aicwf_frame_queue_init(&rx_priv->rxq, 1, MAX_RXQLEN);
#endif
    spin_lock_init(&rx_priv->rxqlock);
#ifdef CONFIG_PREALLOC_RX_SKB
	spin_lock_init(&rx_priv->rxbuff_lock);
	//aicwf_prealloc_init();
#endif
    atomic_set(&rx_priv->rx_cnt, 0);

#ifdef CONFIG_USB_MSG_IN_EP
	if(rx_priv->usbdev->msg_in_pipe){
    	aicwf_frame_queue_init(&rx_priv->msg_rxq, 1, MAX_RXQLEN);
    	spin_lock_init(&rx_priv->msg_rxqlock);
    	atomic_set(&rx_priv->msg_rx_cnt, 0);
	}
#endif


#ifdef AICWF_RX_REORDER
    INIT_LIST_HEAD(&rx_priv->rxframes_freequeue);
    spin_lock_init(&rx_priv->freeq_lock);
    rx_priv->recv_frames = aicwf_rxframe_queue_init(&rx_priv->rxframes_freequeue, MAX_REORD_RXFRAME);
    if (!rx_priv->recv_frames) {
        txrx_err("no enough buffer for free recv frame queue!\n");
        kfree(rx_priv);
        return NULL;
    }
	reord_init(rx_priv);
#endif

    return rx_priv;
}

#ifdef AICWF_RX_REORDER
static void aicwf_recvframe_queue_deinit(struct list_head *q)
{
    struct recv_msdu *req, *next;

    list_for_each_entry_safe(req, next, q, rxframe_list) {
        list_del_init(&req->rxframe_list);
    }
}
#endif
void aicwf_rx_deinit(struct aicwf_rx_priv* rx_priv)
{
	if (!rx_priv)
		return;
#ifdef AICWF_RX_REORDER
	/* Stop publication before joining RX producers or retired objects. */
	reord_stop(rx_priv);
#endif
	AICWFDBG(LOGINFO, "stio rx thread\n");
#ifdef AICWF_SDIO_SUPPORT
    if (rx_priv->sdiodev->bus_if->busrx_thread) {
        complete_all(&rx_priv->sdiodev->bus_if->busrx_trgg);
        kthread_stop(rx_priv->sdiodev->bus_if->busrx_thread);
        rx_priv->sdiodev->bus_if->busrx_thread = NULL;
    }
#endif
#ifdef AICWF_USB_SUPPORT

#ifdef CONFIG_RX_TASKLET//AIDEN tasklet
	if (rx_priv->usbdev->rx_tasklet_initialized) {
		tasklet_kill(&rx_priv->usbdev->recv_tasklet);
		rx_priv->usbdev->rx_tasklet_initialized = false;
	}
#endif
    if (rx_priv->usbdev->bus_if->busrx_thread) {
        complete_all(&rx_priv->usbdev->bus_if->busrx_trgg);
		kthread_stop(rx_priv->usbdev->bus_if->busrx_thread);
        rx_priv->usbdev->bus_if->busrx_thread = NULL;
    }
#ifdef CONFIG_USB_MSG_IN_EP
	if(rx_priv->usbdev->msg_in_pipe){
		if (rx_priv->usbdev->bus_if->msg_busrx_thread) {
			complete_all(&rx_priv->usbdev->bus_if->msg_busrx_trgg);
			kthread_stop(rx_priv->usbdev->bus_if->msg_busrx_thread);
			rx_priv->usbdev->bus_if->msg_busrx_thread = NULL;
		}
	}
#endif
#ifdef CONFIG_USB_RX_REASSEMBLE
    if (rx_priv->rx_reassemble_skb) {
        aicwf_dev_skb_free(rx_priv->rx_reassemble_skb);
    }
    if (rx_priv->rx_msg_reassemble_skb) {
        aicwf_dev_skb_free(rx_priv->rx_msg_reassemble_skb);
    }
#endif

#endif

#ifdef CONFIG_USB_MSG_IN_EP
	if(rx_priv->usbdev->msg_in_pipe){
		aicwf_frame_queue_flush(&rx_priv->msg_rxq);
	}
#endif

#ifdef CONFIG_PREALLOC_RX_SKB
	rxbuff_queue_flush(rx_priv);
#else
	aicwf_frame_queue_flush(&rx_priv->rxq);
#endif

#ifdef AICWF_RX_REORDER
	/* All producer threads/tasklets are gone; join every asynchronous retire. */
	reord_shutdown(rx_priv);
    aicwf_recvframe_queue_deinit(&rx_priv->rxframes_freequeue);
    if (rx_priv->recv_frames)
        vfree(rx_priv->recv_frames);
#endif

#ifdef CONFIG_PREALLOC_RX_SKB
	//aicwf_prealloc_exit();
#endif
    kfree(rx_priv);
	AICWFDBG(LOGINFO, "%s Exit\n", __func__);

}

bool aicwf_rxframe_enqueue(struct device *dev, struct frame_queue *q, struct sk_buff *pkt)
{
    return aicwf_frame_enq(dev, q, pkt, 0);
}


void aicwf_dev_skb_free(struct sk_buff *skb)
{
    if (!skb)
        return;

    dev_kfree_skb_any(skb);
}

static struct sk_buff *aicwf_frame_queue_penq(struct frame_queue *pq, int prio, struct sk_buff *p)
{
    struct sk_buff_head *q;

    if (pq->queuelist[prio].qlen >= pq->qmax)
        return NULL;

    q = &pq->queuelist[prio];
    __skb_queue_tail(q, p);
    pq->qcnt++;
    if (pq->hi_prio < prio)
        pq->hi_prio = (u16)prio;

    return p;
}

void aicwf_frame_queue_flush(struct frame_queue *pq)
{
    int prio;
    struct sk_buff_head *q;
    struct sk_buff *p, *next;

    for (prio = 0; prio < pq->num_prio; prio++)
    {
        q = &pq->queuelist[prio];
        skb_queue_walk_safe(q, p, next) {
            skb_unlink(p, q);
            aicwf_dev_skb_free(p);
            pq->qcnt--;
        }
    }
}

void aicwf_frame_queue_init(struct frame_queue *pq, int num_prio, int max_len)
{
    int prio;

    memset(pq, 0, offsetof(struct frame_queue, queuelist) + (sizeof(struct sk_buff_head) * num_prio));
    pq->num_prio = (u16)num_prio;
    pq->qmax = (u16)max_len;

    for (prio = 0; prio < num_prio; prio++) {
        skb_queue_head_init(&pq->queuelist[prio]);
    }
}

struct sk_buff *aicwf_frame_queue_peek_tail(struct frame_queue *pq, int *prio_out)
{
    int prio;

    if (pq->qcnt == 0)
        return NULL;

    for (prio = 0; prio < pq->hi_prio; prio++)
        if (!skb_queue_empty(&pq->queuelist[prio]))
            break;

    if (prio_out)
        *prio_out = prio;

    return skb_peek_tail(&pq->queuelist[prio]);
}

bool aicwf_is_framequeue_empty(struct frame_queue *pq)
{
    int prio, len = 0;

    for (prio = 0; prio <= pq->hi_prio; prio++)
        len += pq->queuelist[prio].qlen;

    if(len > 0)
        return false;
    else
        return true;
}

struct sk_buff *aicwf_frame_dequeue(struct frame_queue *pq)
{
    struct sk_buff_head *q;
    struct sk_buff *p;
    int prio;

    if (pq->qcnt == 0)
        return NULL;

    while ((prio = pq->hi_prio) > 0 && skb_queue_empty(&pq->queuelist[prio]))
        pq->hi_prio--;

    q = &pq->queuelist[prio];
    p = __skb_dequeue(q);
    if (p == NULL)
        return NULL;

    pq->qcnt--;

    return p;
}

static struct sk_buff *aicwf_skb_dequeue_tail(struct frame_queue *pq, int prio)
{
    struct sk_buff_head *q = &pq->queuelist[prio];
    struct sk_buff *p = skb_dequeue_tail(q);

    if (!p)
        return NULL;

    pq->qcnt--;

    return p;
}

bool aicwf_frame_enq(struct device *dev, struct frame_queue *q, struct sk_buff *pkt, int prio)
{
    struct sk_buff *p = NULL;
    int prio_modified = -1;

    if (q->queuelist[prio].qlen < q->qmax && q->qcnt < q->qmax) {
        aicwf_frame_queue_penq(q, prio, pkt);
        return true;
    }
    if (q->queuelist[prio].qlen >= q->qmax) {
        prio_modified = prio;
    } else if (q->qcnt >= q->qmax) {
        p = aicwf_frame_queue_peek_tail(q, &prio_modified);
        if (prio_modified > prio)
            return false;
    }

    if (prio_modified >= 0) {
        if (prio_modified == prio)
            return false;

        p = aicwf_skb_dequeue_tail(q, prio_modified);
        aicwf_dev_skb_free(p);

        p = aicwf_frame_queue_penq(q, prio_modified, pkt);
        if (p == NULL)
            txrx_err("failed\n");
    }

    return p != NULL;
}

#ifdef CONFIG_PREALLOC_RX_SKB
void rxbuff_free(struct rx_buff *rxbuff)
{
   kfree(rxbuff->data);
   kfree(rxbuff);
}

struct rx_buff *rxbuff_queue_penq(struct rx_frame_queue *pq, struct rx_buff *p)
{

    struct list_head *q;
    if (pq->qcnt >= pq->qmax)
        return NULL;

    q = &pq->queuelist;
    list_add_tail(&p->queue,q);

    pq->qcnt++;

    return p;
}

struct rx_buff *rxbuff_dequeue(struct rx_frame_queue *pq)
{
    struct rx_buff *p = NULL;

    if (pq->qcnt == 0) {
        printk("%s %d, rxq is empty\n", __func__, __LINE__);
        return NULL;
    }

    if(list_empty(&pq->queuelist)) {
        printk("%s %d, rxq is empty\n", __func__, __LINE__);
        return NULL;
    } else {
        p = list_first_entry(&pq->queuelist, struct rx_buff, queue);
        list_del_init(&p->queue);
        pq->qcnt--;
    }

    return p;
}

bool aicwf_rxbuff_enqueue(struct device *dev, struct rx_frame_queue *rxq, struct rx_buff *pkt)
{
//    struct rx_buff *p = NULL;

    if ((rxq == NULL) || (pkt == NULL)) {
        printk("%s %d, rxq or pkt is NULL\n", __func__, __LINE__);
        return false;
    }

    if (rxq->qcnt < rxq->qmax) {
        if (rxbuff_queue_penq(rxq, pkt)) {
            return true;
        } else {
            printk("%s %d, rxbuff enqueue fail\n", __func__, __LINE__);
            return false;
        }
    } else {
        printk("%s %d, rxq or pkt is full\n", __func__, __LINE__);
        return false;
    }
}
#endif

