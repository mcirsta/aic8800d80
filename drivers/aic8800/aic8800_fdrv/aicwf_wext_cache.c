// SPDX-License-Identifier: GPL-2.0
#include <linux/etherdevice.h>
#include "rwnx_defs.h"
#include "aicwf_wext_linux.h"

static bool aicwf_wext_cache_active_locked(struct rwnx_hw *hw, u64 generation)
{
	return !hw->wext_cache_stopped && generation &&
	       generation == hw->wext_cache_generation && hw->wext_scan &&
	       hw->scan_request == &hw->wext_scan->request &&
	       hw->wext_cache_owner == hw->wext_scan->request.wdev;
}

u64 aicwf_wext_cache_generation(struct rwnx_hw *hw)
{
	u64 generation;

	spin_lock_bh(&hw->cb_lock);
	generation = hw->wext_cache_generation;
	if (!aicwf_wext_cache_active_locked(hw, generation))
		generation = 0;
	spin_unlock_bh(&hw->cb_lock);
	return generation;
}

void aicwf_wext_result_put(struct scanu_result_wext *entry)
{
	if (refcount_dec_and_test(&entry->refs))
		kfree(entry);
}

void aicwf_wext_cache_free_list(struct list_head *entries)
{
	struct scanu_result_wext *entry, *next;

	list_for_each_entry_safe(entry, next, entries, scanu_re_list) {
		list_del_init(&entry->scanu_re_list);
		aicwf_wext_result_put(entry);
	}
}

void aicwf_wext_cache_reset_locked(struct rwnx_hw *hw,
				    struct wireless_dev *wdev,
				    struct list_head *retired)
{
	list_splice_tail_init(&hw->wext_scanre_list, retired);
	hw->wext_cache_count = 0;
	hw->wext_cache_bytes = 0;
	hw->wext_cache_owner = wdev;
	if (!++hw->wext_cache_generation)
		hw->wext_cache_generation++;
}

void aicwf_wext_cache_purge(struct rwnx_hw *hw, struct wireless_dev *wdev,
			    bool stop)
{
	LIST_HEAD(retired);

	spin_lock_bh(&hw->cb_lock);
	if (stop)
		hw->wext_cache_stopped = true;
	if (!wdev || hw->wext_cache_owner == wdev)
		aicwf_wext_cache_reset_locked(hw, NULL, &retired);
	spin_unlock_bh(&hw->cb_lock);
	aicwf_wext_cache_free_list(&retired);
}

void aicwf_wext_cache_add(struct rwnx_hw *hw, u64 generation,
			  const u8 *bssid, u16 capability, u16 center_freq,
			  s8 rssi, const u8 *ies, size_t ies_len)
{
	struct scanu_result_wext *entry, *old = NULL, *iter;
	size_t bytes, retained;

	if (!generation || ies_len > IEEE80211_MAX_DATA_LEN)
		return;
	bytes = struct_size(entry, ies, ies_len);
	entry = kmalloc(bytes, GFP_ATOMIC);
	if (!entry)
		return;
	INIT_LIST_HEAD(&entry->scanu_re_list);
	refcount_set(&entry->refs, 1);
	entry->capability = capability;
	entry->center_freq = center_freq;
	entry->rssi = rssi;
	entry->ies_len = ies_len;
	ether_addr_copy(entry->bssid, bssid);
	if (ies_len)
		memcpy(entry->ies, ies, ies_len);

	spin_lock_bh(&hw->cb_lock);
	if (!aicwf_wext_cache_active_locked(hw, generation))
		goto reject;
	list_for_each_entry(iter, &hw->wext_scanre_list, scanu_re_list) {
		if (iter->center_freq == center_freq &&
		    ether_addr_equal(iter->bssid, bssid)) {
			old = iter;
			break;
		}
	}
	retained = hw->wext_cache_bytes;
	if (old)
		retained -= struct_size(old, ies, old->ies_len);
	if ((!old && hw->wext_cache_count >= AICWF_WEXT_CACHE_MAX_RESULTS) ||
	    bytes > AICWF_WEXT_CACHE_MAX_BYTES - retained)
		goto reject;
	if (old) {
		list_replace_init(&old->scanu_re_list, &entry->scanu_re_list);
	} else {
		list_add_tail(&entry->scanu_re_list, &hw->wext_scanre_list);
		hw->wext_cache_count++;
	}
	hw->wext_cache_bytes = retained + bytes;
	spin_unlock_bh(&hw->cb_lock);
	if (old)
		aicwf_wext_result_put(old);
	return;

reject:
	spin_unlock_bh(&hw->cb_lock);
	aicwf_wext_result_put(entry);
}

int aicwf_wext_cache_snapshot(struct rwnx_hw *hw,
			      struct scanu_result_wext **entries, size_t capacity)
{
	struct scanu_result_wext *entry;
	int count = 0;

	spin_lock_bh(&hw->cb_lock);
	if (hw->wext_cache_stopped) {
		count = -ESHUTDOWN;
		goto out;
	}
	if (capacity < hw->wext_cache_count) {
		count = -E2BIG;
		goto out;
	}
	list_for_each_entry(entry, &hw->wext_scanre_list, scanu_re_list) {
		refcount_inc(&entry->refs);
		entries[count++] = entry;
	}
out:
	spin_unlock_bh(&hw->cb_lock);
	return count;
}
