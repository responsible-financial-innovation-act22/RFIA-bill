/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright(c) 2007 - 2011 Realtek Corporation. */

#ifndef _RTW_HT_H_
#define _RTW_HT_H_

#include "osdep_service.h"
#include "wifi.h"

struct ht_priv {
	u32	ht_option;
	u32	ampdu_enable;/* for enable Tx A-MPDU */
	u32	tx_amsdu_enable;/* for enable Tx A-MSDU */
	u32	tx_amdsu_maxlen; /*  1: 8k, 0:4k ; default:8k, for tx */
	u32	rx_ampdu_maxlen; /* for rx reordering ctrl win_sz,
				  * updated when join_callback. */
	u8	bwmode;/*  */
	u8	ch_offset;/* PRIME_CHNL_OFFSET */
	u8	sgi;/* short GI */

	/* for processing Tx A-MPDU */
	u8	agg_enable_bitmap;
	u8	candidate_tid_bitmap;

	struct ieee80211_ht_cap ht_cap;
};

#endif	/* _RTL871X_HT_H_ */
