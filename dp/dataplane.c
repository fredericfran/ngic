/*
 * Copyright (c) 2017 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arpa/inet.h>

#include <rte_cycles.h>
#include <rte_ip.h>
#include <rte_ip_frag.h>
#include <rte_errno.h>

#include "main.h"
#include "interface.h"
#include "epc_packet_framework.h"
#include "gtpu.h"
#include "ipv4.h"
#include "ether.h"
#include "util.h"
#include "meter.h"
#include <sponsdn.h>
#include <stdbool.h>

struct rte_hash *rte_uplink_hash;
struct rte_hash *rte_downlink_hash;
struct rte_hash *rte_adc_hash;
struct rte_hash *rte_adc_ue_hash;
struct rte_hash *rte_sess_hash;
struct rte_hash *rte_ue_hash;


void
gtpu_decap(struct rte_mbuf **pkts, uint32_t n,
		uint64_t *pkts_mask)
{
	uint32_t i;
	int ret = 0;
	static uint64_t ul_num_dcap;
	struct ipv4_hdr *ipv4_hdr;
	struct udp_hdr *udp_hdr;
	struct gtpu_hdr *gtpu_hdr;
	struct epc_meta_data *meta_data;

	for (i = 0; i < n; i++) {
		/* reject if not with s1u ip */
		ipv4_hdr = get_mtoip(pkts[i]);
		if (ipv4_hdr->dst_addr != app.s1u_ip) {
			RESET_BIT(*pkts_mask, i);
			continue;
		}

		/* reject un-tunneled packet */
		udp_hdr = get_mtoudp(pkts[i]);
		if (ntohs(udp_hdr->dst_port) != UDP_PORT_GTPU) {
			RESET_BIT(*pkts_mask, i);
			continue;
		}

		gtpu_hdr = get_mtogtpu(pkts[i]);
		if (gtpu_hdr->teid == 0 || gtpu_hdr->msgtype != GTP_GPDU) {
			RESET_BIT(*pkts_mask, i);
			continue;
		}


		meta_data =
		(struct epc_meta_data *)RTE_MBUF_METADATA_UINT8_PTR(pkts[i],
						META_DATA_OFFSET);
		meta_data->teid = ntohl(gtpu_hdr->teid);
		meta_data->enb_ipv4 = ntohl(ipv4_hdr->src_addr);
		RTE_LOG(DEBUG, DP, "Received tunneled packet with teid 0x%x\n",
				meta_data->teid);
		RTE_LOG(DEBUG, DP, "From Ue IP " IPV4_ADDR "\n",
				IPV4_ADDR_FORMAT(gtpu_inner_src_ip(pkts[i])));

		ret = decap_gtpu_hdr(pkts[i]);

		if (ret < 0)
			RESET_BIT(*pkts_mask, i);
		ul_num_dcap++;

	}
}

void
gtpu_encap(struct dp_session_info **sess_info, struct rte_mbuf **pkts,
		uint32_t n, uint64_t *pkts_mask, uint64_t *pkts_queue_mask)
{
	uint32_t i;
	struct dp_session_info *si;
	struct rte_mbuf *m;
	uint16_t len;

	for (i = 0; i < n; i++) {
		si = sess_info[i];
		m = pkts[i];

		if (!ISSET_BIT(*pkts_mask, i))
			continue;

		if (si == NULL) {
			RESET_BIT(*pkts_mask, i);
			continue;
		}

		if (!si->dl_s1_info.enb_teid) {
			RESET_BIT(*pkts_mask, i);
			SET_BIT(*pkts_queue_mask, i);
			continue;
		}

		if (encap_gtpu_hdr(m, si->dl_s1_info.enb_teid) < 0) {
			RESET_BIT(*pkts_mask, i);
			continue;
		}

		len = rte_pktmbuf_data_len(m);
		len = len - ETH_HDR_SIZE;

		/* construct iphdr */
		construct_ipv4_hdr(m, len, IP_PROTO_UDP, ntohl(app.s1u_ip),
				si->dl_s1_info.enb_addr.u.ipv4_addr);

		len = len - IPv4_HDR_SIZE;
		/* construct udphdr */
		construct_udp_hdr(m, len, UDP_PORT_GTPU, UDP_PORT_GTPU);
	}
}

void
ul_sess_info_get(struct rte_mbuf **pkts, uint32_t n, uint32_t *res,
		uint64_t *pkts_mask, struct dp_sdf_per_bearer_info **sess_info)
{
	uint32_t j;
	struct ul_bm_key key[MAX_BURST_SZ];
	void *key_ptr[MAX_BURST_SZ];
	struct epc_meta_data *meta_data;
	uint64_t hit_mask = 0;

	for (j = 0; j < n; j++) {
		meta_data =
		(struct epc_meta_data *)RTE_MBUF_METADATA_UINT8_PTR(pkts[j],
							META_DATA_OFFSET);
		key[j].s1u_sgw_teid = meta_data->teid;
		key[j].rid = res[j];
		RTE_LOG(DEBUG, DP, "BEAR_SESS LKUP:UL_KEY teid:%u, rid:%u\n",
				key[j].s1u_sgw_teid, key[j].rid);
		key_ptr[j] = &key[j];
	}
	if ((iface_lookup_uplink_bulk_data((const void **)&key_ptr[0], n,
			&hit_mask, (void **)sess_info)) < 0)
		hit_mask = 0;

	for (j = 0; j < n; j++) {
		if (!ISSET_BIT(hit_mask, j)) {
			RESET_BIT(*pkts_mask, j);
			RTE_LOG(DEBUG, DP, "SDF BEAR LKUP:FAIL!! UL_KEY "
				"teid:%u, rid:%u\n",
				key[j].s1u_sgw_teid, key[j].rid);
			sess_info[j] = NULL;
		}
	}
}

void
adc_ue_info_get(struct rte_mbuf **pkts, uint32_t n, uint32_t *res,
		void **adc_ue_info, uint32_t flow)
{
	uint32_t j;
	struct dl_bm_key key[MAX_BURST_SZ];
	struct ipv4_hdr *ipv4_hdr;
	void *key_ptr[MAX_BURST_SZ];
	uint64_t hit_mask = 0;

	for (j = 0; j < n; j++) {
		ipv4_hdr = get_mtoip(pkts[j]);
		key[j].rid = res[j];
		if (flow == UL_FLOW)
			key[j].ue_ipv4 = ntohl(ipv4_hdr->src_addr);
		else
			key[j].ue_ipv4 = ntohl(ipv4_hdr->dst_addr);

		key_ptr[j] = &key[j];
	}

	if ((rte_hash_lookup_bulk_data(rte_adc_ue_hash,
		(const void **)&key_ptr[0], n, &hit_mask, adc_ue_info)) < 0)
		RTE_LOG(ERR, DP, "ADC UE Bulk LKUP:FAIL!!\n");

	for (j = 0; j < n; j++)
		if (!ISSET_BIT(hit_mask, j))
			adc_ue_info[j] = NULL;
}


void
dl_sess_info_get(struct rte_mbuf **pkts, uint32_t n, uint32_t *res,
		uint64_t *pkts_mask, struct dp_sdf_per_bearer_info **sess_info,
		struct dp_session_info **si)
{
	uint32_t j;
	struct ipv4_hdr *ipv4_hdr;
	struct dl_bm_key *key_ptr[MAX_BURST_SZ];
	uint64_t hit_mask = 0;

	for (j = 0; j < n; j++) {
		ipv4_hdr = get_mtoip(pkts[j]);
		struct epc_meta_data *meta_data =
		(struct epc_meta_data *)RTE_MBUF_METADATA_UINT8_PTR(pkts[j],
							META_DATA_OFFSET);
		meta_data->key.ue_ipv4 = ntohl(ipv4_hdr->dst_addr);
		meta_data->key.rid = res[j];
		RTE_LOG(DEBUG, DP, "BEAR_SESS LKUP:DL_KEY ue_addr:"IPV4_ADDR
				", rid:%u\n",
				IPV4_ADDR_HOST_FORMAT(meta_data->key.ue_ipv4),
				meta_data->key.rid);
		key_ptr[j] = &(meta_data->key);
	}
	if ((iface_lookup_downlink_bulk_data((const void **)&key_ptr[0], n,
			&hit_mask, (void **)sess_info)) < 0)
		RTE_LOG(ERR, DP, "SDF BEAR Bulk LKUP:FAIL!!\n");

	for (j = 0; j < n; j++) {
		if (!ISSET_BIT(hit_mask, j)) {
			RESET_BIT(*pkts_mask, j);
			RTE_LOG(DEBUG, DP, "SDF BEAR LKUP FAIL!! DL_KEY "
					"ue_addr:"IPV4_ADDR", rid:%u\n",
				IPV4_ADDR_HOST_FORMAT((key_ptr[j])->ue_ipv4),
				((struct dl_bm_key *)key_ptr[j])->rid);
			sess_info[j] = NULL;
			si[j] = NULL;
		} else {
			si[j] = sess_info[j]->bear_sess_info;
		}
	}
}

void
get_pcc_info(void **sess_info, uint32_t n, void **pcc_info)
{
	uint32_t i;
	struct dp_sdf_per_bearer_info *psdf;

	for (i = 0; i < n; i++) {
		psdf = (struct dp_sdf_per_bearer_info *)sess_info[i];
		if (psdf == NULL) {
			pcc_info[i] = NULL;
			continue;
		}
		pcc_info[i] = &psdf->pcc_info;
	}
}

void
pcc_gating(struct dp_sdf_per_bearer_info **sdf_info,
		uint32_t n, uint64_t *pkts_mask)
{
	struct dp_pcc_rules *pcc;
	struct dp_sdf_per_bearer_info *psdf;
	uint32_t i;

	for (i = 0; i < n; i++) {
		psdf = (struct dp_sdf_per_bearer_info *)sdf_info[i];
		if (psdf == NULL)
			continue;
		pcc = &psdf->pcc_info;
		if (pcc == NULL)
			continue;

		if (pcc->gate_status == CLOSE) {
			RESET_BIT(*pkts_mask, i);
			pcc->drop_pkt_count++;
		}
	}
}

/**
 * To map rating group value to index
 * @param rg_val
 *	rating group.
 * @param  rg_idx_map
 *	index map structure.
 *
 * @return
 * rating group index
 */
static uint8_t
get_rg_idx(uint32_t rg_val, struct rating_group_index_map *rg_idx_map)
{
	uint32_t i;
	if (rg_val == 0)
		return MAX_RATING_GRP;
	for (i = 0; i < MAX_RATING_GRP; i++)
		if (rg_idx_map[i].rg_val == rg_val)
			return i;
	return MAX_RATING_GRP;
}

void
get_rating_grp(void **adc_ue_info, void **sdf_info,
		uint32_t **rgrp, uint32_t n)
{
	uint32_t i;
	struct dp_adc_ue_info *adc_ue;
	struct dp_sdf_per_bearer_info *psdf;
	struct dp_pcc_rules *pcc;

	for (i = 0; i < n; i++) {
			adc_ue = adc_ue_info[i];
			if (adc_ue && adc_ue->adc_info.rating_group) {
					rgrp[i] = &adc_ue->adc_info.rating_group;
					continue;
			}
			psdf = (struct dp_sdf_per_bearer_info *)sdf_info[i];
			if (psdf == NULL)
					continue;
			pcc = &psdf->pcc_info;
			if (pcc && pcc->rating_group)
					rgrp[i] = &pcc->rating_group;
			else
					rgrp[i] = NULL;
	}
}

static void
update_cdr(struct ipcan_dp_bearer_cdr *cdr, struct rte_mbuf *pkt,
				uint32_t flow, enum pkt_action_t action)
{
	uint32_t charged_len;
	struct ipv4_hdr *ip_h;

	ip_h = rte_pktmbuf_mtod_offset(pkt, struct ipv4_hdr *,
			sizeof(struct ether_hdr));
	charged_len =
			RTE_MIN(rte_pktmbuf_pkt_len(pkt) -
					sizeof(struct ether_hdr),
					ntohs(ip_h->total_length));
	if (action == CHARGED) {
		if (flow == UL_FLOW) {
			cdr->data_vol.ul_cdr.bytes += charged_len;
			cdr->data_vol.ul_cdr.pkt_count++;
		} else {
			cdr->data_vol.dl_cdr.bytes += charged_len;
			cdr->data_vol.dl_cdr.pkt_count++;
		}	/* if (flow == UL_FLOW) */
	} else {
		if (flow == UL_FLOW) {
			cdr->data_vol.ul_drop.bytes += charged_len;
			cdr->data_vol.ul_drop.pkt_count++;
		} else {
			cdr->data_vol.dl_drop.bytes += charged_len;
			cdr->data_vol.dl_drop.pkt_count++;
		}	/* if (flow == UL_FLOW) */
	}
}

#ifdef ADC_UPFRONT
void
update_adc_cdr(void **adc_ue_info,
		struct rte_mbuf **pkts, uint32_t n,
		uint64_t *adc_pkts_mask, uint64_t *pkts_mask,
		uint32_t flow)
{
	uint32_t i;
	struct dp_adc_ue_info *adc_ue;

	for (i = 0; i < n; i++) {
		adc_ue = (struct dp_adc_ue_info *)adc_ue_info[i];
		if (adc_ue == NULL)
			continue;

		/* record cdr counts if ADC rule is open and pkt is not dropped
		 * due to pcc rule of metering.*/
		if ((ISSET_BIT(*adc_pkts_mask, i))
				&& (ISSET_BIT(*pkts_mask, i)))
			update_cdr(&adc_ue->adc_cdr, pkts[i], flow, CHARGED);

		/* record drop counts if ADC rule is hit but gate is closed*/
		if (!(ISSET_BIT(*adc_pkts_mask, i)))
			update_cdr(&adc_ue->adc_cdr, pkts[i], flow, DROPPED);
	}	/* for (i = 0; i < n; i++)*/
}
#endif /* ADC_UPFRONT*/

void
update_sdf_cdr(void **adc_ue_info,
		struct dp_sdf_per_bearer_info **sdf_bear_info,
		struct rte_mbuf **pkts, uint32_t n,
		uint64_t *adc_pkts_mask, uint64_t *pkts_mask,
		uint32_t flow)
{
	uint32_t i;
	struct dp_sdf_per_bearer_info *psdf;
	struct dp_adc_ue_info *adc_ue;

	for (i = 0; i < n; i++) {
		psdf = sdf_bear_info[i];
		if (psdf == NULL)
			continue;
		/* if ADC rule is hit, but gate is closed
		 * then don't update PCC cdr. */
		adc_ue = (struct dp_adc_ue_info *)adc_ue_info[i];
		if ((adc_ue != NULL)
				&& !ISSET_BIT(*adc_pkts_mask, i))
			continue;

		/* if ADC CDR is updated, then no need to
		 * update PCC cdr */
		if (ISSET_BIT(*adc_pkts_mask, i))
			continue;

		if (ISSET_BIT(*pkts_mask, i))
			update_cdr(&psdf->sdf_cdr, pkts[i], flow, CHARGED);
		else
			update_cdr(&psdf->sdf_cdr, pkts[i], flow, DROPPED);
	}	/* for (i = 0; i < n; i++)*/
}

void
update_bear_cdr(struct dp_sdf_per_bearer_info **sdf_bear_info,
		struct rte_mbuf **pkts, uint32_t n,
		uint64_t *pkts_mask, uint32_t flow)
{
	uint32_t i;
	struct dp_session_info *si;
	struct dp_sdf_per_bearer_info *psdf;

	for (i = 0; i < n; i++) {
		psdf = sdf_bear_info[i];
		if (psdf == NULL)
			continue;

		si = psdf->bear_sess_info;
		if (si == NULL)
			continue;

		if (ISSET_BIT(*pkts_mask, i))
			update_cdr(&si->ipcan_dp_bearer_cdr, pkts[i],
					flow, CHARGED);
		else
			update_cdr(&si->ipcan_dp_bearer_cdr, pkts[i],
					flow, DROPPED);
	}	/* for (i = 0; i < n; i++)*/
}

void
update_rating_grp_cdr(void **sess_info, uint32_t **rgrp,
		struct rte_mbuf **pkts, uint32_t n,
		uint64_t *pkts_mask, uint32_t flow)
{
	uint32_t i;
	struct dp_session_info *si;
	struct dp_sdf_per_bearer_info *psdf;
	uint8_t rg_idx;

	for (i = 0; i < n; i++) {
		psdf = (struct dp_sdf_per_bearer_info *)sess_info[i];
		if (psdf == NULL)
			continue;

		si = psdf->bear_sess_info;
		if (si == NULL)
			continue;

		if (rgrp[i] == NULL)
			continue;

		rg_idx = get_rg_idx(*rgrp[i], si->ue_info_ptr->rg_idx_map);
		if (rg_idx >= MAX_RATING_GRP)
			continue;

		if (ISSET_BIT(*pkts_mask, i))
			update_cdr(&si->ue_info_ptr->rating_grp[rg_idx],
					pkts[i], flow, CHARGED);
		else
			update_cdr(&si->ue_info_ptr->rating_grp[rg_idx],
					pkts[i], flow, DROPPED);
	}	/* for (i = 0; i < n; i++)*/
}

#ifdef SDF_MTR
void
get_sdf_mtr_id(void **sess_info, void **mtr_id,
					uint64_t **mtr_drops, uint32_t n)
{
	uint32_t i;
	struct dp_sdf_per_bearer_info *psdf;

	for (i = 0; i < n; i++) {
		if (!ISSET_BIT(*pkts_mask, i))
			continue;
		psdf = (struct dp_sdf_per_bearer_info *)sess_info[i];
		if (psdf == NULL) {
			mtr_id[i] = NULL;
			continue;
		}
		mtr_id[i] = &psdf->sdf_mtr_obj;
		mtr_drops[i] = &psdf->sdf_mtr_drops;
		RTE_LOG(DEBUG, DP, "SDF MTR LKUP: mtr_obj:0x%"PRIx64"\n",
				(uint64_t)&psdf->sdf_mtr_obj);
	}
}
#endif /* SDF_MTR */
#ifdef APN_MTR
void
get_apn_mtr_id(void **sess_info, void **mtr_id,
					uint64_t **mtr_drops, uint32_t n)
{
	uint32_t i;
	struct dp_session_info *si;
	struct dp_sdf_per_bearer_info *psdf;
	struct ue_session_info *ue;

	for (i = 0; i < n; i++) {
		if (!ISSET_BIT(*pkts_mask, i))
			continue;
		psdf = (struct dp_sdf_per_bearer_info *)sess_info[i];
		if (psdf == NULL) {
			mtr_id[i] = NULL;
			continue;
		}
		si = psdf->bear_sess_info;
		ue = si->ue_info_ptr;
		mtr_id[i] = &ue->apn_mtr_obj;
		mtr_drops[i] = &ue->apn_mtr_drops;
		RTE_LOG(DEBUG, DP, "BEAR_SESS MTR LKUP: apn_mtr_id:%u, "
				"apn_mtr_obj:0x%"PRIx64"\n",
				si->apn_mtr_idx, (uint64_t)mtr_id[i]);
	}
}
#endif /* APN_MTR */
void
adc_hash_lookup(struct rte_mbuf **pkts, uint32_t n, uint32_t *rid, uint8_t flow)
{
	uint32_t j;
	uint32_t key32[MAX_BURST_SZ];
	uint32_t *key_ptr[MAX_BURST_SZ];
	uint64_t hit_mask = 0;
	struct msg_adc *data[MAX_BURST_SZ];
	struct ipv4_hdr *ipv4_hdr;

	for (j = 0; j < n; j++) {
		ipv4_hdr = get_mtoip(pkts[j]);
		key32[j] = (flow == UL_FLOW) ? ipv4_hdr->dst_addr :
				ipv4_hdr->src_addr;
		key_ptr[j] = &key32[j];
	}

	if (iface_lookup_adc_bulk_data((const void **)key_ptr,
			n, &hit_mask, (void **)data) < 0)
		hit_mask = 0;

	for (j = 0; j < n; j++) {
		if (ISSET_BIT(hit_mask, j)) {
			RTE_LOG(DEBUG, DP, "ADC_DNS_LKUP: rid[%d]:%u\n", j,
					data[j]->rule_id);
			rid[j] = data[j]->rule_id;
		} else {
			rid[j] = 0;
		}
	}
}

static inline bool is_dns_pkt(struct rte_mbuf *m, uint32_t rid)
{
	struct ipv4_hdr *ip_hdr;
	struct ether_hdr *eth_hdr;

	eth_hdr = rte_pktmbuf_mtod(m, struct ether_hdr *);
	ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);

	if (rte_ipv4_frag_pkt_is_fragmented(ip_hdr))
		return false;

	if (rid != DNS_RULE_ID)
		return false;

	return true;
}

void
update_dns_meta(struct rte_mbuf **pkts, uint32_t n, uint32_t *rid)
{
	uint32_t i;
	struct epc_meta_data *meta_data;
	for (i = 0; i < n; i++) {

		meta_data =
			(struct epc_meta_data *)RTE_MBUF_METADATA_UINT8_PTR(
					pkts[i], META_DATA_OFFSET);

		if (likely(!is_dns_pkt(pkts[i], rid[i]))) {
			meta_data->dns = 0;
			continue;
		}

		meta_data->dns = 1;
	}
}

static int
get_worker_index(unsigned lcore_id)
{
	return epc_app.worker_core_mapping[lcore_id];
}

void
clone_dns_pkts(struct rte_mbuf **pkts, uint32_t n, uint64_t pkts_mask)
{
	uint32_t i;
	struct epc_meta_data *meta_data;
	unsigned lcore_id = rte_lcore_id();
	int worker_index = get_worker_index(lcore_id);

	for (i = 0; i < n; i++) {
		if (ISSET_BIT(pkts_mask, i)) {
			meta_data =
			(struct epc_meta_data *)RTE_MBUF_METADATA_UINT8_PTR(
						pkts[i], META_DATA_OFFSET);
			if (meta_data->dns) {
				push_dns_ring(pkts[i]);
				++(epc_app.worker[worker_index].
						num_dns_packets);
			}
		}
	}
}

void
update_nexthop_info(struct rte_mbuf **pkts, uint32_t n,
		uint64_t *pkts_mask, uint8_t portid)
{
	uint32_t i;
	for (i = 0; i < n; i++) {
		if (ISSET_BIT(*pkts_mask, i)) {
			if (construct_ether_hdr(pkts[i], portid) < 0)
				RESET_BIT(*pkts_mask, i);
		}
		/* TODO: Set checksum offload.*/
	}
}

void
update_adc_rid_from_domain_lookup(uint32_t *rb, uint32_t *rc, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (rc[i] != 0)
			rb[i] = rc[i];
}


/**
 * @brief create hash table.
 *
 */
int
hash_create(const char *name, struct rte_hash **rte_hash,
		uint32_t entries, uint32_t key_len)
{
	struct rte_hash_parameters rte_hash_params = {
		.name = name,
		.entries = entries,
		.key_len = key_len,
		.hash_func = DEFAULT_HASH_FUNC,
		.hash_func_init_val = 0,
		.socket_id = rte_socket_id(),
	};

	*rte_hash = rte_hash_create(&rte_hash_params);
	if (*rte_hash == NULL)
		rte_exit(EXIT_FAILURE, "%s hash create failed: %s (%u)\n",
			rte_hash_params.name,
			rte_strerror(rte_errno), rte_errno);
	return 0;
}

void init_hash(void)
{
	int ret;

	/*
	 * Create Uplink DB
	 */
	hash_create("iface_uplink_db", &rte_uplink_hash,
				LDB_ENTRIES_DEFAULT * HASH_SIZE_FACTOR,
				sizeof(struct ul_bm_key));
	/*
	 * Create Downlink DB
	 */
	hash_create("iface_downlink_db", &rte_downlink_hash,
				LDB_ENTRIES_DEFAULT * HASH_SIZE_FACTOR,
				sizeof(struct dl_bm_key));

	/*
	 * Create ADC Domain Hash table
	 */
	hash_create("adc_domain_hash", &rte_adc_hash, LDB_ENTRIES_DEFAULT,
			sizeof(uint32_t));

	/*
	 * Create ADC UE info Hash table
	 */
	hash_create("adc_ue_info", &rte_adc_ue_hash, LDB_ENTRIES_DEFAULT,
			sizeof(struct dl_bm_key));

	/*
	 * Create UE Sess Hash table
	 */
	hash_create("ue_sess_info", &rte_ue_hash, LDB_ENTRIES_DEFAULT,
			sizeof(uint32_t));

	/* Create table for sponsored domain names */
	ret = epc_sponsdn_create(DEFAULT_DN_NUM);
	if (ret)
		rte_exit(EXIT_FAILURE,
			"error allocating sponsored DN context %d\n", ret);
	/*
	 * Init callback APIs
	 */
	app_sess_tbl_init();
	app_pcc_tbl_init();
	app_mtr_tbl_init();
	app_filter_tbl_init();
	app_adc_tbl_init();
}