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

#ifndef PACKET_FILTERS_H
#define PACKET_FILTERS_H

/**
 * @file
 *
 * Contains functions to initialize, manage, and install packet filters internal
 * to the Control Plane as well as calls to forward the installed packet filters
 * to the Data Plane.
 */

#include <stdint.h>
#include <netinet/in.h>

#include "ue.h"

#define FIRST_FILTER_ID 1

#define STATIC_PCC_FILE "../config/static_pcc.cfg"
#define ADC_RULE_FILE "../config/adc_rules.cfg"

typedef struct packet_filter_t {
	uint8_t direction;
	uint32_t precedence;
	struct in_addr remote_ip_addr;
	uint8_t remote_ip_mask;
	uint16_t remote_port_low;
	uint16_t remote_port_high;
	uint8_t proto;
	uint8_t proto_mask;
	struct in_addr local_ip_addr;
	uint8_t local_ip_mask;
	uint16_t local_port_low;
	uint16_t local_port_high;
	uint16_t rating_group;
} packet_filter;

extern const packet_filter catch_all;

void
push_all_packet_filters(void);

void
push_packet_filter(uint16_t index);


/**
 * Installs a packet filter in the CP & DP.
 * @param new_packet_filter
 *   A packet filter yet to be installed
 * @param dl_mbr
 *   downlink maximum bit rate
 * @return
 *   \- >= 0 - on success - indicates index of packet filter
 *   \- < 0 - on error
 */
int
install_packet_filter(const packet_filter *new_packet_filter,
		uint64_t dl_mbr);

/**
 * Returns the packet filter index.
 * @param pf
 *   Packet filter
 * @return
 *   Packet filter index matching packet filter 'pf'
 */
int
get_packet_filter_id(const packet_filter *pf);

/**
 * Clears the packet filter at '*pf' to accept all packets.
 * @param pf
 *   The packet filter to reset
 */
void
reset_packet_filter(packet_filter *pf);

/**
 * Returns direction of packet filter (uplink and/or downlink).
 * @param index
 *   Packet filter index
 * @return
 *   Direction as defined as tft packet filter direction in 3gpp 24.008
 *   table 10.5.162, one of:
 *   \- TFT_DIRECTION_BIDIRECTIONAL
 *   \- TFT_DIRECTION_UPLINK_ONLY
 *   \- TFT_DIRECTION_DOWNLINK_ONLY
 */
uint8_t
get_packet_filter_direction(uint16_t index);

/**
 * Returns the packet filter given it's index.
 * @param index
 *   Index of packet filter
 * @return
 *   Packet filter at index
 */
packet_filter *
get_packet_filter(uint16_t index);

/**
 * Packet filter initialization function. Reads static file and populates
 * packet filters accordingly.
 */
void
init_packet_filters(void);

void parse_adc_rules(void);

#endif