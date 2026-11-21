#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/netdevice.h>
#include <net/netlink.h>
#include <linux/wireless.h>
#include <linux/nl80211.h>
#include <net/iw_handler.h>
#include <uapi/linux/if_arp.h>
#include <linux/vmalloc.h>
#include "aicwf_debug.h"
#include "rwnx_msg_tx.h"
#include "aicwf_wext_linux.h"
#include "rwnx_defs.h"
#include "rwnx_main.h"

#define WLAN_CAPABILITY_ESS		(1<<0)
#define WLAN_CAPABILITY_IBSS	(1<<1)
#define WLAN_CAPABILITY_PRIVACY (1<<4)

#define IEEE80211_HT_CAP_SGI_20			0x0020
#define IEEE80211_HT_CAP_SGI_40			0x0040

#define IEEE80211_HE_CH_BW_SET_160_80P80 		1 << 4
#define IEEE80211_HE_CH_BW_SET_160_IN_5G 		1 << 3
#define IEEE80211_HE_CH_BW_SET_40_AND_80_IN_5G 	1 << 2
#define IEEE80211_HE_CH_BW_SET_40_IN_2_4G 		1 << 1

#if WIRELESS_EXT < 17
	#define IW_QUAL_QUAL_INVALID   0x10
	#define IW_QUAL_LEVEL_INVALID  0x20
	#define IW_QUAL_NOISE_INVALID  0x40
	#define IW_QUAL_QUAL_UPDATED   0x1
	#define IW_QUAL_LEVEL_UPDATED  0x2
	#define IW_QUAL_NOISE_UPDATED  0x4
#endif


/*				20/40/80,	ShortGI,	MCS Rate  */
const u16 VHT_MCS_DATA_RATE[3][2][30] = {
	{	
		{
			13, 26, 39, 52, 78, 104, 117, 130, 156, 156,
			26, 52, 78, 104, 156, 208, 234, 260, 312, 312,
			39, 78, 117, 156, 234, 312, 351, 390, 468, 520
		},	/* Long GI, 20MHz */
		{
			14, 29, 43, 58, 87, 116, 130, 144, 173, 173,
			29, 58, 87, 116, 173, 231, 260, 289, 347, 347,
			43,	87, 130, 173, 260, 347, 390,	433,	520, 578
		}
	},		/* Short GI, 20MHz */
	{	
		{
			27, 54, 81, 108, 162, 216, 243, 270, 324, 360,
			54, 108, 162, 216, 324, 432, 486, 540, 648, 720,
			81, 162, 243, 324, 486, 648, 729, 810, 972, 1080
		}, 	/* Long GI, 40MHz */
		{
			30, 60, 90, 120, 180, 240, 270, 300, 360, 400,
			60, 120, 180, 240, 360, 480, 540, 600, 720, 800,
			90, 180, 270, 360, 540, 720, 810, 900, 1080, 1200
		}
	},		/* Short GI, 40MHz */
	{	
		{
			59, 117,  176, 234, 351, 468, 527, 585, 702, 780,
			117, 234, 351, 468, 702, 936, 1053, 1170, 1404, 1560,
			176, 351, 527, 702, 1053, 1404, 1580, 1755, 2106, 2340
		},	/* Long GI, 80MHz */
		{
			65, 130, 195, 260, 390, 520, 585, 650, 780, 867,
			130, 260, 390, 520, 780, 1040, 1170, 1300, 1560, 1734,
			195, 390, 585, 780, 1170, 1560, 1755, 1950, 2340, 2600
		}	/* Short GI, 80MHz */
	}	
};


/*HE 20/40/80,MCS Rate  */
const u16 HE_MCS_DATA_RATE[3][30] = {
	{
		9, 17, 26, 34, 52, 69, 77, 86, 103, 115,
		129, 143, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	},	/* 20MHz */
	{
		17, 34, 52, 69, 103, 138, 155, 172, 207, 229,
		258, 286, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	}, 	/* 40MHz */
	{
		36, 72,  108, 144, 216, 288, 324, 360, 432, 480,
		540, 601, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	},	/* 80MHz */
	
};


#if WIRELESS_EXT >= 17
struct iw_statistics iwstats;

static struct iw_statistics *aicwf_get_wireless_stats(struct net_device *dev)
{
	int tmp_level = -100;
	int tmp_qual = 0;
	int tmp_noise = 0;
	bool signal_valid = false;

	struct rwnx_vif* rwnx_vif = netdev_priv(dev);
	struct rwnx_hw* rwnx_hw = rwnx_vif->rwnx_hw;
	struct rwnx_sta *sta = READ_ONCE(rwnx_vif->sta.ap);

	struct mm_get_sta_info_cfm cfm;

	AICWFDBG(LOGTRACE, "%s Enter", __func__);

	if (sta) {
		if (!rwnx_send_get_sta_info_req(rwnx_hw, sta->sta_idx, &cfm)) {
			tmp_level = cfm.rssi;
			signal_valid = true;
		}
	}


	iwstats.qual.level = tmp_level;
	iwstats.qual.qual = tmp_qual;
	iwstats.qual.noise = tmp_noise;
	iwstats.qual.updated = 0x07;
	iwstats.qual.updated |= IW_QUAL_DBM;
	if (!signal_valid)
		iwstats.qual.updated |= IW_QUAL_LEVEL_INVALID;

	return &iwstats;
}
#endif


static int aicwf_get_name(struct net_device *dev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	struct rwnx_vif *vif = netdev_priv(dev);
	struct rwnx_sta *sta = READ_ONCE(vif->sta.ap);
	struct mm_get_sta_info_cfm cfm;
	union rwnx_rate_ctrl_info rate;
	const char *name;
	int ret;

	AICWFDBG(LOGTRACE, "%s Enter", __func__);

	if (!sta) {
		snprintf(wrqu->name, IFNAMSIZ, "unassociated");
		return 0;
	}

	ret = rwnx_send_get_sta_info_req(vif->rwnx_hw, sta->sta_idx, &cfm);
	if (ret)
		return ret;

	rate.value = cfm.rate_info;
	switch (rate.formatModTx) {
	case FORMATMOD_NON_HT:
	case FORMATMOD_NON_HT_DUP_OFDM:
		name = "IEEE 802.11bg";
		break;
	case FORMATMOD_HT_MF:
	case FORMATMOD_HT_GF:
		name = "IEEE 802.11n";
		break;
	case FORMATMOD_VHT:
		name = "IEEE 802.11ac";
		break;
	case FORMATMOD_HE_MU:
	case FORMATMOD_HE_SU:
	case FORMATMOD_HE_ER:
		name = "IEEE 802.11ax";
		break;
	default:
		return -ENODATA;
	}

	snprintf(wrqu->name, IFNAMSIZ, "%s", name);
	return 0;
}

static int aicwf_get_freq(struct net_device *dev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	struct rwnx_vif* rwnx_vif = netdev_priv(dev);


	AICWFDBG(LOGTRACE, "%s Enter", __func__);

	if(rwnx_vif->sta.ap){
		wrqu->freq.m = rwnx_vif->sta.ap->center_freq * 100000;
		wrqu->freq.e = 1;
		wrqu->freq.i = ieee80211_frequency_to_channel(rwnx_vif->sta.ap->center_freq);
	}else{
		wrqu->freq.m = 2412 * 100000;
		wrqu->freq.e = 1;
		wrqu->freq.i = 1;
	}

	return 0;
}

static int aicwf_get_mode(struct net_device *dev, struct iw_request_info *a,
			   union iwreq_data *wrqu, char *b)
{

	AICWFDBG(LOGTRACE, "%s Enter", __func__);

	wrqu->mode = IW_MODE_AUTO;
#if 0
	if (check_fwstate(pmlmepriv, WIFI_STATION_STATE) == _TRUE)
		wrqu->mode = IW_MODE_INFRA;
	else if ((check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE) == _TRUE) ||
		 (check_fwstate(pmlmepriv, WIFI_ADHOC_STATE) == _TRUE))

		wrqu->mode = IW_MODE_ADHOC;
	else if (check_fwstate(pmlmepriv, WIFI_AP_STATE) == _TRUE)
		wrqu->mode = IW_MODE_MASTER;
	else if (check_fwstate(pmlmepriv, WIFI_MONITOR_STATE) == _TRUE)
		wrqu->mode = IW_MODE_MONITOR;
	else
		wrqu->mode = IW_MODE_AUTO;
#endif

	return 0;

}


static int aicwf_get_range(struct net_device *dev,
			    struct iw_request_info *info,
			    union iwreq_data *wrqu, char *extra)
{
	struct iw_range *range = (struct iw_range *)extra;
	u16 val = 0;

	AICWFDBG(LOGTRACE, "%s Enter", __func__);


	wrqu->data.length = sizeof(*range);
	memset(range, 0, sizeof(*range));

	/* Let's try to keep this struct in the same order as in
	 * linux/include/wireless.h
	 */

	/* TODO: See what values we can set, and remove the ones we can't
	 * set, or fill them with some default data.
	 */

	/* ~5 Mb/s real (802.11b) */
	range->throughput = 5 * 1000 * 1000;

	/* TODO: Not used in 802.11b?
	*	range->min_nwid;	 Minimal NWID we are able to set  */
	/* TODO: Not used in 802.11b?
	*	range->max_nwid;	 Maximal NWID we are able to set  */

	/* Old Frequency (backward compat - moved lower ) */
	/*	range->old_num_channels;
	 *	range->old_num_frequency;
	 * 	range->old_freq[6];  Filler to keep "version" at the same offset  */

	/* signal level threshold range */

	/* Quality of link & SNR stuff */
	/* Quality range (link, level, noise)
	 * If the quality is absolute, it will be in the range [0 ; max_qual],
	 * if the quality is dBm, it will be in the range [max_qual ; 0].
	 * Don't forget that we use 8 bit arithmetics...
	 *
	 * If percentage range is 0~100
	 * Signal strength dbm range logical is -100 ~ 0
	 * but usually value is -90 ~ -20
	 */
	range->max_qual.qual = 100;
#ifdef CONFIG_SIGNAL_DISPLAY_DBM
	range->max_qual.level = (u8)-100;
	range->max_qual.noise = (u8)-100;
	range->max_qual.updated = IW_QUAL_ALL_UPDATED; /* Updated all three */
	range->max_qual.updated |= IW_QUAL_DBM;
#else /* !CONFIG_SIGNAL_DISPLAY_DBM */
	/* percent values between 0 and 100. */
	range->max_qual.level = 100;
	range->max_qual.noise = 100;
	range->max_qual.updated = IW_QUAL_ALL_UPDATED; /* Updated all three */
#endif /* !CONFIG_SIGNAL_DISPLAY_DBM */

	/* This should contain the average/typical values of the quality
	 * indicator. This should be the threshold between a "good" and
	 * a "bad" link (example : monitor going from green to orange).
	 * Currently, user space apps like quality monitors don't have any
	 * way to calibrate the measurement. With this, they can split
	 * the range between 0 and max_qual in different quality level
	 * (using a geometric subdivision centered on the average).
	 * I expect that people doing the user space apps will feedback
	 * us on which value we need to put in each driver... */
	range->avg_qual.qual = 92; /* > 8% missed beacons is 'bad' */
#ifdef CONFIG_SIGNAL_DISPLAY_DBM
	/* TODO: Find real 'good' to 'bad' threshold value for RSSI */
	range->avg_qual.level = (u8)-70;
	range->avg_qual.noise = 0;
	range->avg_qual.updated = IW_QUAL_ALL_UPDATED; /* Updated all three */
	range->avg_qual.updated |= IW_QUAL_DBM;
#else /* !CONFIG_SIGNAL_DISPLAY_DBM */
	/* TODO: Find real 'good' to 'bad' threshol value for RSSI */
	range->avg_qual.level = 30;
	range->avg_qual.noise = 100;
	range->avg_qual.updated = IW_QUAL_ALL_UPDATED; /* Updated all three */
#endif /* !CONFIG_SIGNAL_DISPLAY_DBM */
#if 0
	range->num_bitrates = RATE_COUNT;

	for (i = 0; i < RATE_COUNT && i < IW_MAX_BITRATES; i++)
		range->bitrate[i] = rtw_rates[i];

	range->min_frag = MIN_FRAG_THRESHOLD;
	range->max_frag = MAX_FRAG_THRESHOLD;
#endif

	range->pm_capa = 0;

	range->we_version_compiled = WIRELESS_EXT;
	range->we_version_source = 16;

	/*	range->retry_capa;	 What retry options are supported
	 *	range->retry_flags;	 How to decode max/min retry limit
	 *	range->r_time_flags;	 How to decode max/min retry life
	 *	range->min_retry;	 Minimal number of retries
	 *	range->max_retry;	 Maximal number of retries
	 *	range->min_r_time;	 Minimal retry lifetime
	 *	range->max_r_time;	 Maximal retry lifetime  */
#if 0 
	for (i = 0, val = 0; i < rfctl->max_chan_nums; i++) {

		/* Include only legal frequencies for some countries */
		if (rfctl->channel_set[i].ChannelNum != 0) {
			range->freq[val].i = rfctl->channel_set[i].ChannelNum;
			range->freq[val].m = rtw_ch2freq(rfctl->channel_set[i].ChannelNum) * 100000;
			range->freq[val].e = 1;
			val++;
		}

		if (val == IW_MAX_FREQUENCIES)
			break;
	}
#endif
	range->num_channels = val;
	range->num_frequency = val;

	/* Commented by Albert 2009/10/13
	 * The following code will proivde the security capability to network manager.
	 * If the driver doesn't provide this capability to network manager,
	 * the WPA/WPA2 routers can't be choosen in the network manager. */

	/*
	#define IW_SCAN_CAPA_NONE		0x00
	#define IW_SCAN_CAPA_ESSID		0x01
	#define IW_SCAN_CAPA_BSSID		0x02
	#define IW_SCAN_CAPA_CHANNEL	0x04
	#define IW_SCAN_CAPA_MODE		0x08
	#define IW_SCAN_CAPA_RATE		0x10
	#define IW_SCAN_CAPA_TYPE		0x20
	#define IW_SCAN_CAPA_TIME		0x40
	*/

#if WIRELESS_EXT > 17
	range->enc_capa = IW_ENC_CAPA_WPA | IW_ENC_CAPA_WPA2 |
			  IW_ENC_CAPA_CIPHER_TKIP | IW_ENC_CAPA_CIPHER_CCMP;
#endif

#ifdef IW_SCAN_CAPA_ESSID /* WIRELESS_EXT > 21 */
	range->scan_capa = IW_SCAN_CAPA_ESSID | IW_SCAN_CAPA_TYPE | IW_SCAN_CAPA_BSSID |
		   IW_SCAN_CAPA_CHANNEL | IW_SCAN_CAPA_MODE | IW_SCAN_CAPA_RATE;
#endif



	return 0;

}


static const u8 *aicwf_wext_scan_ie(const struct scanu_result_wext *entry,
				   u8 id, u8 min_len)
{
	const u8 *ie = cfg80211_find_ie(id, entry->ies, entry->ies_len);

	return ie && ie[1] >= min_len ? ie : NULL;
}

static bool aicwf_wext_scan_has_he(const struct scanu_result_wext *entry)
{
	const u8 *ie = entry->ies;
	size_t left = entry->ies_len;

	while (left >= 2 && ie[1] <= left - 2) {
		/* Extension ID, MAC/PHY capabilities and the mandatory MCS maps. */
		if (ie[0] == WLAN_EID_EXTENSION && ie[1] >= 22 &&
		    ie[2] == WLAN_EID_EXT_HE_CAPABILITY)
			return true;
		left -= ie[1] + 2;
		ie += ie[1] + 2;
	}
	return false;
}

static u32 aicwf_wext_scan_rate(const struct scanu_result_wext *entry)
{
	struct rate_info rate = {};
	const struct ieee80211_ht_cap *ht;
	const struct ieee80211_vht_cap *vht;
	const u8 *ie;
	u32 max_rate = 0;
	u32 value;
	u16 cap = 0, map;
	int i, j;

	for (i = 0; i < 2; i++) {
		ie = aicwf_wext_scan_ie(entry, i ? WLAN_EID_EXT_SUPP_RATES :
					WLAN_EID_SUPP_RATES, 1);
		if (!ie)
			continue;
		for (j = 0; j < ie[1]; j++)
			max_rate = max(max_rate, (ie[j + 2] & 0x7f) * 500000U);
	}

	ie = aicwf_wext_scan_ie(entry, WLAN_EID_HT_CAPABILITY, sizeof(*ht));
	if (ie) {
		ht = (const void *)(ie + 2);
		cap = le16_to_cpu(ht->cap_info);
		rate.flags = RATE_INFO_FLAGS_MCS;
		rate.bw = cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40 ?
			RATE_INFO_BW_40 : RATE_INFO_BW_20;
		if (cap & (rate.bw == RATE_INFO_BW_40 ? IEEE80211_HT_CAP_SGI_40 :
						     IEEE80211_HT_CAP_SGI_20))
			rate.flags |= RATE_INFO_FLAGS_SHORT_GI;
		/* Equal-modulation HT MCS rates supported by cfg80211. */
		for (i = 0; i < 32; i++) {
			if (!(ht->mcs.rx_mask[i / 8] & BIT(i % 8)))
				continue;
			rate.mcs = i;
			value = cfg80211_calculate_bitrate(&rate);
			if (value <= INT_MAX / 100000U)
				max_rate = max(max_rate, value * 100000U);
		}
	}

	ie = aicwf_wext_scan_ie(entry, WLAN_EID_VHT_CAPABILITY, sizeof(*vht));
	if (ie) {
		vht = (const void *)(ie + 2);
		rate.flags = RATE_INFO_FLAGS_VHT_MCS;
		rate.bw = entry->center_freq > 4000 ? RATE_INFO_BW_80 :
			  cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40 ?
			  RATE_INFO_BW_40 : RATE_INFO_BW_20;
		if ((rate.bw == RATE_INFO_BW_80 &&
		     (le32_to_cpu(vht->vht_cap_info) & IEEE80211_VHT_CAP_SHORT_GI_80)) ||
		    (rate.bw == RATE_INFO_BW_40 && (cap & IEEE80211_HT_CAP_SGI_40)) ||
		    (rate.bw == RATE_INFO_BW_20 && (cap & IEEE80211_HT_CAP_SGI_20)))
			rate.flags |= RATE_INFO_FLAGS_SHORT_GI;
		map = le16_to_cpu(vht->supp_mcs.rx_mcs_map);
		for (i = 0; i < 8; i++) {
			j = (map >> (2 * i)) & 3;
			if (j == 3)
				continue;
			rate.mcs = 7 + j;
			rate.nss = i + 1;
			value = cfg80211_calculate_bitrate(&rate);
			if (value <= INT_MAX / 100000U)
				max_rate = max(max_rate, value * 100000U);
		}
	}
	/* A scan result is not a negotiated rate; do not invent an HE bitrate. */
	return max_rate;
}

static int aicwf_wext_scan_event(struct iw_request_info *info, char **stream,
				 char *end, struct iw_event *iwe, int len)
{
	char *next = iwe_stream_add_event(info, *stream, end, iwe, len);

	if (next == *stream)
		return -E2BIG;
	*stream = next;
	return 0;
}

static int aicwf_wext_scan_point(struct iw_request_info *info, char **stream,
				 char *end, struct iw_event *iwe, const void *data)
{
	char *next = iwe_stream_add_point(info, *stream, end, iwe,
					       (char *)(data ? data : ""));

	if (next == *stream)
		return -E2BIG;
	*stream = next;
	return 0;
}

static int aicwf_wext_scan_security(struct iw_request_info *info,
				   const struct scanu_result_wext *entry,
				   char **stream, char *end)
{
	static const u8 wpa_oui[] = { 0x00, 0x50, 0xf2, 0x01 };
	static const u8 wps_oui[] = { 0x00, 0x50, 0xf2, 0x04 };
	const u8 *ie = entry->ies;
	size_t left = entry->ies_len;
	char custom[IW_CUSTOM_MAX];
	struct iw_event iwe;
	const char *prefix;
	size_t len, prefix_len;
	bool rsn, wpa, wps;
	bool sent_rsn = false, sent_wpa = false;
	int ret;

	while (left >= 2 && ie[1] <= left - 2) {
		len = ie[1] + 2;
		rsn = ie[0] == WLAN_EID_RSN && !sent_rsn;
		wpa = !sent_wpa && ie[0] == WLAN_EID_VENDOR_SPECIFIC &&
		      ie[1] >= sizeof(wpa_oui) &&
		      !memcmp(ie + 2, wpa_oui, sizeof(wpa_oui));
		wps = ie[0] == WLAN_EID_VENDOR_SPECIFIC && ie[1] >= sizeof(wps_oui) &&
		      !memcmp(ie + 2, wps_oui, sizeof(wps_oui));
		if (rsn || wpa || wps) {
			sent_rsn |= rsn;
			sent_wpa |= wpa;
			if (rsn || wpa) {
				prefix = rsn ? "rsn_ie=" : "wpa_ie=";
				prefix_len = strlen(prefix);
				/* Keep the legacy textual event within its WEXT limit. */
				if (prefix_len + len * 2 <= sizeof(custom)) {
					memcpy(custom, prefix, prefix_len);
					bin2hex(custom + prefix_len, ie, len);
					memset(&iwe, 0, sizeof(iwe));
					iwe.cmd = IWEVCUSTOM;
					iwe.u.data.length = prefix_len + len * 2;
					ret = aicwf_wext_scan_point(info, stream, end,
							      &iwe, custom);
					if (ret)
						return ret;
				}
			}
			memset(&iwe, 0, sizeof(iwe));
			iwe.cmd = IWEVGENIE;
			iwe.u.data.length = len;
			ret = aicwf_wext_scan_point(info, stream, end, &iwe, ie);
			if (ret)
				return ret;
		}
		left -= len;
		ie += len;
	}
	return 0;
}

static int aicwf_wext_format_scan(struct iw_request_info *info,
				   const struct scanu_result_wext *entry,
				   char **stream, char *end)
{
	struct iw_event iwe = {};
	const u8 *ssid, *ht, *vht;
	const char *protocol;
	int ret;

	iwe.cmd = SIOCGIWAP;
	iwe.u.ap_addr.sa_family = ARPHRD_ETHER;
	memcpy(iwe.u.ap_addr.sa_data, entry->bssid, ETH_ALEN);
	ret = aicwf_wext_scan_event(info, stream, end, &iwe, IW_EV_ADDR_LEN);
	if (ret)
		return ret;

	ssid = aicwf_wext_scan_ie(entry, WLAN_EID_SSID, 0);
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWESSID;
	iwe.u.data.flags = 1;
	iwe.u.data.length = ssid ? min_t(u8, ssid[1], IEEE80211_MAX_SSID_LEN) : 0;
	ret = aicwf_wext_scan_point(info, stream, end, &iwe, ssid ? ssid + 2 : NULL);
	if (ret)
		return ret;

	ht = aicwf_wext_scan_ie(entry, WLAN_EID_HT_CAPABILITY,
				 sizeof(struct ieee80211_ht_cap));
	vht = aicwf_wext_scan_ie(entry, WLAN_EID_VHT_CAPABILITY,
				  sizeof(struct ieee80211_vht_cap));
	if (aicwf_wext_scan_has_he(entry))
		protocol = "IEEE 802.11ax";
	else if (vht)
		protocol = "IEEE 802.11ac";
	else if (entry->center_freq > 4000)
		protocol = ht ? "IEEE 802.11an" : "IEEE 802.11a";
	else
		protocol = ht ? "IEEE 802.11bgn" : "IEEE 802.11bg";
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWNAME;
	snprintf(iwe.u.name, IFNAMSIZ, "%s", protocol);
	ret = aicwf_wext_scan_event(info, stream, end, &iwe, IW_EV_CHAR_LEN);
	if (ret)
		return ret;

	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWFREQ;
	iwe.u.freq.m = entry->center_freq;
	iwe.u.freq.e = 6;
	iwe.u.freq.i = ieee80211_frequency_to_channel(entry->center_freq);
	ret = aicwf_wext_scan_event(info, stream, end, &iwe, IW_EV_FREQ_LEN);
	if (ret)
		return ret;

	if (entry->capability & (WLAN_CAPABILITY_IBSS | WLAN_CAPABILITY_ESS)) {
		memset(&iwe, 0, sizeof(iwe));
		iwe.cmd = SIOCGIWMODE;
		iwe.u.mode = entry->capability & WLAN_CAPABILITY_ESS ?
			IW_MODE_MASTER : IW_MODE_ADHOC;
		ret = aicwf_wext_scan_event(info, stream, end, &iwe, IW_EV_UINT_LEN);
		if (ret)
			return ret;
	}

	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWENCODE;
	iwe.u.data.flags = entry->capability & WLAN_CAPABILITY_PRIVACY ?
		IW_ENCODE_ENABLED | IW_ENCODE_NOKEY : IW_ENCODE_DISABLED;
	ret = aicwf_wext_scan_point(info, stream, end, &iwe, NULL);
	if (ret)
		return ret;

	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWRATE;
	iwe.u.bitrate.value = aicwf_wext_scan_rate(entry);
	ret = aicwf_wext_scan_event(info, stream, end, &iwe, IW_EV_PARAM_LEN);
	if (ret)
		return ret;

	ret = aicwf_wext_scan_security(info, entry, stream, end);
	if (ret)
		return ret;
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = IWEVQUAL;
	iwe.u.qual.updated = IW_QUAL_QUAL_UPDATED | IW_QUAL_LEVEL_UPDATED |
			     IW_QUAL_NOISE_INVALID | IW_QUAL_DBM;
	iwe.u.qual.level = (u8)entry->rssi;
	iwe.u.qual.qual = 100;
	return aicwf_wext_scan_event(info, stream, end, &iwe, IW_EV_QUAL_LEN);
}


static int aicwf_get_wap(struct net_device *dev,
			  struct iw_request_info *info,
			  union iwreq_data *wrqu, char *extra)
{

	struct rwnx_vif* rwnx_vif = netdev_priv(dev);


	AICWFDBG(LOGTRACE, "%s Enter", __func__);
	
	wrqu->ap_addr.sa_family = ARPHRD_ETHER;

	memset(wrqu->ap_addr.sa_data, 0, ETH_ALEN);

	if(rwnx_vif->sta.ap){
		memcpy(wrqu->ap_addr.sa_data, rwnx_vif->sta.ap->mac_addr, ETH_ALEN);
	}else{
		memset(wrqu->ap_addr.sa_data, 0, ETH_ALEN);
	}

	return 0;

}


/* One caller-owned allocation includes the SSID and the flexible channel tail. */
static struct rwnx_wext_scan *
aicwf_wext_scan_alloc(struct net_device *dev, union iwreq_data *wrqu, char *extra)
{
	struct rwnx_vif *vif = netdev_priv(dev);
	struct rwnx_hw *hw = vif->rwnx_hw;
	struct wiphy *wiphy = hw->wiphy;
	struct cfg80211_scan_request *request;
	const struct iw_scan_req *iwreq = NULL;
	struct rwnx_wext_scan *scan;
	bool passive = false;
	int count = hw->support_freqs_number;
	int i;

	if (!wiphy || count <= 0 || count > ARRAY_SIZE(hw->support_freqs))
		return ERR_PTR(-EINVAL);
	if (wrqu->data.flags & ~IW_SCAN_THIS_ESSID)
		return ERR_PTR(-EOPNOTSUPP);
	if (wrqu->data.length) {
		if (wrqu->data.length != sizeof(*iwreq) || !extra)
			return ERR_PTR(-EINVAL);
		iwreq = (const struct iw_scan_req *)extra;
		if (iwreq->scan_type != IW_SCAN_TYPE_ACTIVE &&
		    iwreq->scan_type != IW_SCAN_TYPE_PASSIVE)
			return ERR_PTR(-EINVAL);
		passive = iwreq->scan_type == IW_SCAN_TYPE_PASSIVE;
	}
	if (wrqu->data.flags & IW_SCAN_THIS_ESSID) {
		if (!iwreq || iwreq->essid_len > IEEE80211_MAX_SSID_LEN)
			return ERR_PTR(-EINVAL);
		if (passive)
			return ERR_PTR(-EOPNOTSUPP);
	}

	scan = kzalloc(struct_size(scan, request.channels, count), GFP_KERNEL);
	if (!scan)
		return ERR_PTR(-ENOMEM);
	init_completion(&scan->done);
	scan->result = -EINPROGRESS;
	request = &scan->request;
	request->wiphy = wiphy;
	request->wdev = &vif->wdev;
	request->ssids = &scan->ssid;
	/* A zero-length SSID requests active wildcard probes; zero SSIDs is passive. */
	request->n_ssids = passive ? 0 : 1;
	if (wrqu->data.flags & IW_SCAN_THIS_ESSID) {
		scan->ssid.ssid_len = iwreq->essid_len;
		memcpy(scan->ssid.ssid, iwreq->essid, iwreq->essid_len);
	}
	for (i = 0; i < count; i++) {
		struct ieee80211_channel *chan;

		chan = ieee80211_get_channel(wiphy, hw->support_freqs[i]);
		if (!chan || (chan->flags & IEEE80211_CHAN_DISABLED))
			continue;
		request->channels[request->n_channels++] = chan;
	}
	if (!request->n_channels) {
		kfree(scan);
		return ERR_PTR(-EINVAL);
	}
	return scan;
}

static int aicwf_set_scan(struct net_device *dev, struct iw_request_info *a,
			  union iwreq_data *wrqu, char *extra)
{
	struct rwnx_vif *vif = netdev_priv(dev);
	struct rwnx_hw *hw = vif->rwnx_hw;
	struct rwnx_wext_scan *scan;
	LIST_HEAD(retired);
	bool submitted = false;
	long waited;
	int ret;

	if (!vif->up || !netif_running(dev))
		return -ENETDOWN;
	if (testmode ||
	    ((RWNX_VIF_TYPE(vif) == NL80211_IFTYPE_STATION ||
	      RWNX_VIF_TYPE(vif) == NL80211_IFTYPE_P2P_CLIENT) &&
	     vif->sta.external_auth) ||
	    atomic_read(&vif->drv_conn_state) == RWNX_DRV_STATUS_CONNECTING ||
	    hw->plat->wait_disconnect_cb)
		return -EBUSY;
	scan = aicwf_wext_scan_alloc(dev, wrqu, extra);
	if (IS_ERR(scan))
		return PTR_ERR(scan);

	/* Do not cancel or replace another caller's scan, including a drain fence. */
	spin_lock_bh(&hw->cb_lock);
	if (hw->wext_cache_stopped || hw->scan_request || hw->scanning ||
	    hw->wext_scan) {
		ret = hw->wext_cache_stopped ? -ESHUTDOWN : -EBUSY;
		spin_unlock_bh(&hw->cb_lock);
		kfree(scan);
		return ret;
	}
	aicwf_wext_cache_reset_locked(hw, &vif->wdev, &retired);
	hw->scan_request = &scan->request;
	hw->wext_scan = scan;
	hw->scanning = true;
	hw->scan_submitted = false;
	spin_unlock_bh(&hw->cb_lock);
	aicwf_wext_cache_free_list(&retired);

	/* Completion only detaches/signals; it never frees the sender's storage. */
	ret = rwnx_send_scanu_req(hw, vif, &scan->request, &submitted);
	if (ret) {
		if (rwnx_wext_scan_abort(hw, scan, ret, submitted) && submitted)
			rwnx_send_scanu_cancel_req(hw, NULL);
	} else {
		waited = wait_for_completion_killable_timeout(&scan->done,
							    msecs_to_jiffies(5000));
		if (waited <= 0 &&
		    rwnx_wext_scan_abort(hw, scan,
					 waited ? (int)waited : -ETIMEDOUT, true))
			rwnx_send_scanu_cancel_req(hw, NULL);
	}

	/* Synchronize with the completer before the caller's final free. */
	spin_lock_bh(&hw->cb_lock);
	ret = scan->result;
	spin_unlock_bh(&hw->cb_lock);
	kfree(scan);
	return ret;
}

static int aicwf_get_scan(struct net_device *dev, struct iw_request_info *info,
			  union iwreq_data *wrqu, char *extra)
{
	struct rwnx_vif *vif = netdev_priv(dev);
	struct scanu_result_wext *entries[AICWF_WEXT_CACHE_MAX_RESULTS];
	char *stream = extra;
	char *end = extra + wrqu->data.length;
	int count, i, ret = 0;

	count = aicwf_wext_cache_snapshot(vif->rwnx_hw, entries, ARRAY_SIZE(entries));
	if (count < 0)
		return count;
	for (i = 0; i < count; i++) {
		if (!ret)
			ret = aicwf_wext_format_scan(info, entries[i], &stream, end);
		aicwf_wext_result_put(entries[i]);
	}
	wrqu->data.length = stream - extra;
	wrqu->data.flags = 0;
	return ret;
}


static int aicwf_get_essid(struct net_device *dev,
			    struct iw_request_info *a,
			    union iwreq_data *wrqu, char *extra)
{
	int ret = 0;
	struct rwnx_vif* rwnx_vif = netdev_priv(dev);
	struct rwnx_hw* rwnx_hw = rwnx_vif->rwnx_hw;
	
	AICWFDBG(LOGTRACE, "%s Enter", __func__);

	wrqu->essid.length = strlen(rwnx_hw->wext_essid);
	memcpy(extra, rwnx_hw->wext_essid, strlen(rwnx_hw->wext_essid));
	wrqu->essid.flags = 1;


	return ret;

}


static int aicwf_get_nick(struct net_device *dev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
			   
	AICWFDBG(LOGTRACE, "%s Enter", __func__);

	if (extra) {
		wrqu->data.length = 8;
		wrqu->data.flags = 1;
		memcpy(extra, "AIC@8800", 8);
	}

	return 0;

}

static int aicwf_get_rate(struct net_device *dev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	struct rwnx_vif *vif = netdev_priv(dev);
	struct rwnx_sta *sta = READ_ONCE(vif->sta.ap);
	struct mm_get_sta_info_cfm cfm;
	union rwnx_rate_ctrl_info rate;
	u16 max_rate = 0;
	int ret;

	AICWFDBG(LOGTRACE, "%s Enter", __func__);

	if (sta) {
		ret = rwnx_send_get_sta_info_req(vif->rwnx_hw, sta->sta_idx, &cfm);
		if (ret)
			return ret;

		rate.value = cfm.rate_info;
		switch (rate.formatModTx) {
		case FORMATMOD_NON_HT:
		case FORMATMOD_NON_HT_DUP_OFDM:
			if (rate.mcsIndexTx >= ARRAY_SIZE(tx_legrates_lut_rate))
				return -ENODATA;
			max_rate = tx_legrates_lut_rate[rate.mcsIndexTx] / 10;
			break;
		case FORMATMOD_HT_MF:
		case FORMATMOD_HT_GF:
		case FORMATMOD_VHT:
			if (rate.bwTx >= ARRAY_SIZE(VHT_MCS_DATA_RATE) ||
			    rate.mcsIndexTx >= ARRAY_SIZE(VHT_MCS_DATA_RATE[0][0]))
				return -ENODATA;
			max_rate = VHT_MCS_DATA_RATE[rate.bwTx][1][rate.mcsIndexTx] / 2;
			break;
		case FORMATMOD_HE_MU:
		case FORMATMOD_HE_SU:
		case FORMATMOD_HE_ER:
			if (rate.bwTx >= ARRAY_SIZE(HE_MCS_DATA_RATE) ||
			    rate.mcsIndexTx >= ARRAY_SIZE(HE_MCS_DATA_RATE[0]))
				return -ENODATA;
			max_rate = HE_MCS_DATA_RATE[rate.bwTx][rate.mcsIndexTx];
			break;
		default:
			return -ENODATA;
		}
	}

	wrqu->bitrate.fixed = 0;	/* no auto select */
	wrqu->bitrate.value = max_rate * 1000000;

	return 0;
}


static int aicwf_get_enc(struct net_device *dev,
			  struct iw_request_info *info,
			  union iwreq_data *wrqu, char *keybuf)
{
	int ret = 0;
	struct iw_point *erq = &(wrqu->encoding);

	AICWFDBG(LOGTRACE, "%s Enter", __func__);

	erq->length = 0;
	erq->flags |= IW_ENCODE_ENABLED;


	return ret;

}


static iw_handler aic_handlers[] = {
	NULL,					/* SIOCSIWCOMMIT */
	aicwf_get_name,			/* SIOCGIWNAME */
	NULL,					/* SIOCSIWNWID */
	NULL,					/* SIOCGIWNWID */
	NULL,					/* SIOCSIWFREQ */
	aicwf_get_freq,			/* SIOCGIWFREQ */
	NULL,					/* SIOCSIWMODE */
	aicwf_get_mode,			/* SIOCGIWMODE */
	NULL,					/* SIOCSIWSENS */
	NULL,					/* SIOCGIWSENS */
	NULL,					/* SIOCSIWRANGE */
	aicwf_get_range,		/* SIOCGIWRANGE */
	NULL,					/* SIOCSIWPRIV */
	NULL,					/* SIOCGIWPRIV */
	NULL,					/* SIOCSIWSTATS */
	NULL,					/* SIOCGIWSTATS */
	NULL,					/* SIOCSIWSPY */
	NULL,					/* SIOCGIWSPY */
	NULL,					/* SIOCGIWTHRSPY */
	NULL,					/* SIOCWIWTHRSPY */
	NULL,					/* SIOCSIWAP */
	aicwf_get_wap,			/* SIOCGIWAP */
	NULL,					/* request MLME operation; uses struct iw_mlme */
	NULL,					/* SIOCGIWAPLIST -- depricated */
	aicwf_set_scan,			/* SIOCSIWSCAN */
	aicwf_get_scan,			/* SIOCGIWSCAN */
	NULL,					/* SIOCSIWESSID */
	aicwf_get_essid,		/* SIOCGIWESSID */
	NULL,					/* SIOCSIWNICKN */
	aicwf_get_nick,			/* SIOCGIWNICKN */
	NULL,					/* -- hole -- */
	NULL,					/* -- hole -- */
	NULL,					/* SIOCSIWRATE */
	aicwf_get_rate,			/* SIOCGIWRATE */
	NULL,					/* SIOCSIWRTS */
	NULL,					/* SIOCGIWRTS */
	NULL,					/* SIOCSIWFRAG */
	NULL,					/* SIOCGIWFRAG */
	NULL,					/* SIOCSIWTXPOW */
	NULL,					/* SIOCGIWTXPOW */
	NULL,					/* SIOCSIWRETRY */
	NULL,					/* SIOCGIWRETRY */
	NULL,					/* SIOCSIWENCODE */
	aicwf_get_enc,			/* SIOCGIWENCODE */
	NULL,					/* SIOCSIWPOWER */
	NULL,					/* SIOCGIWPOWER */
	NULL,					/*---hole---*/
	NULL,					/*---hole---*/
	NULL,					/* SIOCSIWGENIE */
	NULL,					/* SIOCGWGENIE */
	NULL,					/* SIOCSIWAUTH */
	NULL,					/* SIOCGIWAUTH */
	NULL,					/* SIOCSIWENCODEEXT */
	NULL,					/* SIOCGIWENCODEEXT */
	NULL,					/* SIOCSIWPMKSA */
	NULL,					/*---hole---*/
};

struct iw_handler_def aic_handlers_def = {
		.standard = aic_handlers,
		.num_standard = sizeof(aic_handlers) / sizeof(iw_handler),
#if WIRELESS_EXT >= 17
		.get_wireless_stats = aicwf_get_wireless_stats,
#endif
};

void aicwf_set_wireless_ext( struct net_device *ndev, struct rwnx_hw *rwnx_hw){

	AICWFDBG(LOGINFO, "%s Enter", __func__);
	
	
	ndev->wireless_handlers = (struct iw_handler_def *)&aic_handlers_def;
}


