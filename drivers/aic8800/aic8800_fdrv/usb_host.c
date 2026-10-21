/**
 * usb_host.c
 *
 * USB host function declarations
 *
 * Copyright (C) AICSemi 2018-2020
 */


#include "usb_host.h"
#include "rwnx_defs.h"
//#include "ipc_compat.h"
#include "rwnx_tx.h"
#include "rwnx_platform.h"
#include "aicwf_debug.h"

/**
 ****************************************************************************************
 */
void aicwf_usb_host_init(struct usb_host_env_tag *env,
                  void *cb,
                  void *shared_env_ptr,
                  void *pthis)
{
    // Reset the environments

    // Reset the Host environment
    memset(env, 0, sizeof(struct usb_host_env_tag));
    // Save the pointer to the register base
    env->pthis = pthis;
}

/**
 ****************************************************************************************
 */
volatile struct txdesc_host *aicwf_usb_host_txdesc_get(struct usb_host_env_tag *env, const int queue_idx)
{
 //   struct ipc_shared_env_tag *shared_env_ptr = env->shared;
    volatile struct txdesc_host *txdesc_free = NULL;
    uint32_t used_idx = env->txdesc_used_idx[queue_idx];
    uint32_t free_idx = env->txdesc_free_idx[queue_idx];

   // ASSERT_ERR(queue_idx < SDIO_TXQUEUE_CNT);
   // ASSERT_ERR((free_idx - used_idx) <= USB_TXDESC_CNT);

    // Check if a free descriptor is available
    if (free_idx != (used_idx + USB_TXDESC_CNT))
    {
        // Get the pointer to the first free descriptor
    //    txdesc_free = shared_env_ptr->txdesc[queue_idx] + (free_idx % IPC_TXDESC_CNT);
    }
    else
    {
        //txdesc_free = NULL;
    }

    return txdesc_free;
}

/**
 ****************************************************************************************
 */
/* The TX lock serializes publication, firmware replies and cancellation. */
int aicwf_usb_host_txdesc_push(struct usb_host_env_tag *env,
			     int queue_idx, struct sk_buff *skb)
{
	struct rwnx_hw *hw = env->pthis;
	u32 tag, slot;
	int i;

	lockdep_assert_held(&hw->tx_lock);
	for (i = 0; i < USB_TXDESC_CNT; i++) {
		tag = env->txdesc_free_idx[queue_idx];
		env->txdesc_free_idx[queue_idx] = (tag + 1) & 0x3fffffff;
		slot = tag % USB_TXDESC_CNT;
		if (env->tx_host_id[queue_idx][slot])
			continue;
		env->tx_host_tag[queue_idx][slot] = tag;
		env->tx_host_id[queue_idx][slot] = (unsigned long)skb;
		return tag;
	}

	return -ENOSPC;
}

struct sk_buff *aicwf_usb_host_txdesc_take(struct usb_host_env_tag *env,
					 u32 tag)
{
	struct rwnx_hw *hw = env->pthis;
	u32 slot = tag % USB_TXDESC_CNT;
	struct sk_buff *skb;

	lockdep_assert_held(&hw->tx_lock);
	/* A late reply must not consume the next occupant of this ring slot. */
	if (env->tx_host_tag[0][slot] != tag)
		return NULL;
	skb = (struct sk_buff *)env->tx_host_id[0][slot];
	env->tx_host_id[0][slot] = 0;
	return skb;
}

void aicwf_usb_host_txdesc_flush(struct usb_host_env_tag *env)
{
	struct rwnx_hw *hw = env->pthis;
	struct sk_buff *skb;
	int i, dropped = 0;

	lockdep_assert_held(&hw->tx_lock);
	for (i = 0; i < USB_TXDESC_CNT; i++) {
		skb = aicwf_usb_host_txdesc_take(env, env->tx_host_tag[0][i]);
		if (skb) {
			rwnx_usb_tx_drop(hw, skb);
			dropped++;
		}
	}
	dev_dbg(hw->dev, "USB TX shutdown: %d pending confirmations discarded\n", dropped);
}

void aicwf_usb_host_tx_cfm_handler(struct usb_host_env_tag *env, u32 *data)
{
	struct rwnx_hw *hw = env->pthis;
	struct rwnx_txhdr *txhdr;
	struct sk_buff *skb;

	spin_lock_bh(&hw->tx_lock);
	skb = aicwf_usb_host_txdesc_take(env, data[1]);
	dev_dbg(hw->dev, "USB TX confirmation tag=%u matched=%d\n", data[1], !!skb);
	if (skb) {
		txhdr = (struct rwnx_txhdr *)skb->data;
		txhdr->hw_hdr.cfm.status.value = data[0];
		rwnx_txdatacfm(hw, skb);
	}
	spin_unlock_bh(&hw->tx_lock);
}

int aicwf_rwnx_usb_platform_init(struct aic_usb_dev *usbdev)
{
    struct rwnx_plat *rwnx_plat = NULL;
    void *drvdata;
    int ret = -ENODEV;

    rwnx_plat = kzalloc(sizeof(struct rwnx_plat), GFP_KERNEL);

    if (!rwnx_plat)
        return -ENOMEM;

//    rwnx_plat->pci_dev = pci_dev;
    rwnx_plat->usbdev = usbdev;

    ret = rwnx_platform_init(rwnx_plat, &drvdata);
#if 0
    pci_set_drvdata(pci_dev, drvdata);

    if (ret)
        rwnx_plat->deinit(rwnx_plat);
#endif
    return ret;
}

