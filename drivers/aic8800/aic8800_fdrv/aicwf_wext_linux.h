/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _AICWF_WEXT_LINUX_H_
#define _AICWF_WEXT_LINUX_H_

#include <linux/if_ether.h>
#include <linux/list.h>
#include <linux/refcount.h>
#include <linux/types.h>

#define AICWF_WEXT_CACHE_MAX_RESULTS 64
#define AICWF_WEXT_CACHE_MAX_BYTES (64 * 1024)

struct rwnx_hw;
struct net_device;
struct wireless_dev;

/* Immutable metadata and IE bytes; the cache and each reader own a reference. */
struct scanu_result_wext {
	struct list_head scanu_re_list;
	refcount_t refs;
	u16 capability;
	u16 center_freq;
	u16 ies_len;
	s8 rssi;
	u8 bssid[ETH_ALEN];
	u8 ies[];
};

u64 aicwf_wext_cache_generation(struct rwnx_hw *hw);
void aicwf_wext_cache_add(struct rwnx_hw *hw, u64 generation,
			  const u8 *bssid, u16 capability, u16 center_freq,
			  s8 rssi, const u8 *ies, size_t ies_len);
int aicwf_wext_cache_snapshot(struct rwnx_hw *hw,
			      struct scanu_result_wext **entries, size_t capacity);
void aicwf_wext_result_put(struct scanu_result_wext *entry);
void aicwf_wext_cache_free_list(struct list_head *entries);
void aicwf_wext_cache_reset_locked(struct rwnx_hw *hw,
				    struct wireless_dev *wdev,
				    struct list_head *retired);
void aicwf_wext_cache_purge(struct rwnx_hw *hw, struct wireless_dev *wdev,
			    bool stop);

void aicwf_set_wireless_ext( struct net_device *ndev, struct rwnx_hw *rwnx_hw);
void aicwf_scan_complete_event(struct net_device *dev);

#endif /* _AICWF_WEXT_LINUX_H_ */

