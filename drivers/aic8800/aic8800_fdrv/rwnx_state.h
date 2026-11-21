/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RWNX_STATE_H_
#define _RWNX_STATE_H_

#include "rwnx_defs.h"
#include <linux/rculist.h>
#ifdef AICWF_USB_SUPPORT
#include "aicwf_usb.h"
#endif

/* Firmware remote slots and BCMC slots are separate queue-index domains. */
static inline unsigned int rwnx_fw_remote_sta_count(struct rwnx_hw *hw)
{
	unsigned int chip = 0;

#ifdef AICWF_USB_SUPPORT
	if (hw->usbdev)
		chip = hw->usbdev->chipid;
#elif defined(AICWF_SDIO_SUPPORT)
	if (hw->sdiodev)
		chip = hw->sdiodev->chipid;
#endif
	if (chip == PRODUCT_ID_AIC8801 ||
	    ((chip == PRODUCT_ID_AIC8800DC || chip == PRODUCT_ID_AIC8800DW) &&
	     chip_id < 3))
		return NX_REMOTE_STA_MAX_FOR_OLD_IC;
	return NX_REMOTE_STA_MAX;
}

static inline bool rwnx_fw_sta_idx_valid(struct rwnx_hw *hw, unsigned int idx)
{
	return idx < rwnx_fw_remote_sta_count(hw);
}

static inline bool rwnx_fw_bcmc_idx_valid(struct rwnx_hw *hw, unsigned int idx)
{
	unsigned int remote = rwnx_fw_remote_sta_count(hw);

	return idx >= remote && idx < remote + NX_VIRT_DEV_MAX &&
	       idx < ARRAY_SIZE(hw->sta_table);
}

static inline bool rwnx_fw_vif_idx_valid(unsigned int idx)
{
	return idx < NX_VIRT_DEV_MAX;
}

static inline bool rwnx_sta_is_valid(const struct rwnx_sta *sta)
{
	return smp_load_acquire(&sta->valid);
}

static inline void rwnx_sta_publish(struct rwnx_sta *sta)
{
	sta->stats.last_rx_valid = false;
	smp_store_release(&sta->valid, true);
}

static inline void rwnx_sta_unpublish(struct rwnx_sta *sta)
{
	WRITE_ONCE(sta->valid, false);
}

/* Caller holds vif_readers SRCU through its last VIF/wdev/netdev use. */
static inline struct rwnx_vif *rwnx_vif_lookup(struct rwnx_hw *hw,
					    unsigned int idx)
{
	struct rwnx_vif *vif;

	if (idx >= ARRAY_SIZE(hw->vif_table))
		return NULL;
	vif = smp_load_acquire(&hw->vif_table[idx]);
	if (!vif || !smp_load_acquire(&vif->up) ||
		READ_ONCE(vif->vif_index) != idx)
		return NULL;
	return vif;
}

/* Publish initialized identity/up state, never overwrite another live slot. */
static inline int rwnx_vif_publish_locked(struct rwnx_hw *hw,
					  struct rwnx_vif *vif,
					  unsigned int idx)
{
	bool vlan = RWNX_VIF_TYPE(vif) == NL80211_IFTYPE_AP_VLAN;

	lockdep_assert_held(&hw->cb_lock);
	if (idx >= ARRAY_SIZE(hw->vif_table) ||
	    (vlan ? idx < NX_VIRT_DEV_MAX : !rwnx_fw_vif_idx_valid(idx)))
		return -EINVAL;
	if (READ_ONCE(hw->vif_table[idx]))
		return -EBUSY;
	vif->vif_index = idx;
	smp_store_release(&vif->up, true);
	smp_store_release(&hw->vif_table[idx], vif);
	return 0;
}

/* A stale vif_index must not clear a different VIF or leave an owned root. */
static inline void rwnx_vif_unpublish_locked(struct rwnx_hw *hw,
					     struct rwnx_vif *vif)
{
	unsigned int i;

	lockdep_assert_held(&hw->cb_lock);
	WRITE_ONCE(vif->up, false);
	for (i = 0; i < ARRAY_SIZE(hw->vif_table); i++)
		if (READ_ONCE(hw->vif_table[i]) == vif)
			smp_store_release(&hw->vif_table[i], NULL);
}

static inline void rwnx_roc_free_callback(struct rcu_head *head)
{
	kfree(container_of(head, struct rwnx_roc_elem, rcu));
}

/* Detach under cb_lock first; borrowed ROC/wdev pointers use vif_readers. */
static inline void rwnx_roc_free(struct rwnx_hw *hw,
				struct rwnx_roc_elem *roc)
{
	if (roc)
		call_srcu(&hw->vif_readers, &roc->rcu, rwnx_roc_free_callback);
}

#endif /* _RWNX_STATE_H_ */
