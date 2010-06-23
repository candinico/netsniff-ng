/*
 * Copyright (C) 2009, 2010  Daniel Borkmann <daniel@netsniff-ng.org> and 
 *                           Emmanuel Roullit <emmanuel@netsniff-ng.org>
 *
 * This program is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation; either version 2 of the License, or (at 
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY 
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 51 Franklin St, Fifth Floor, Boston, MA 02110, USA
 */

#include <stdint.h>
#include <assert.h>

#include <netsniff-ng/packet.h>

static inline void set_pkt_step(struct packet * pkt, uint16_t type)
{
	assert(pkt);
	pkt->pkt[pkt->step++] = type;
}

int parse_packet(uint8_t * raw, uint32_t len, struct packet * pkt)
{
	uint8_t **buffer = &raw;
	uint32_t tmp_len = len;
#error "Compile here"
	info("WTF\n");
	pkt->raw = raw;
	pkt->ethernet_header = get_ethhdr(buffer, &tmp_len);
	set_pkt_step(pkt, ETHERNET);

	/* Parse l2/l3 */
	info("Parse\n");
	switch (get_ethertype(pkt->ethernet_header)) {
	case ETH_P_8021Q:
	case ETH_P_8021QinQ:
		pkt->vlan_header = get_vlan_hdr(buffer, &tmp_len);
		set_pkt_step(pkt, ETH_P_8021Q);
		break;

	case ETH_P_ARP:
		pkt->arp_header = get_arphdr(buffer, &tmp_len);
		set_pkt_step(pkt, ETH_P_ARP);
		break;

	case ETH_P_IP:
		pkt->ip_header = get_iphdr(buffer, &tmp_len);
		set_pkt_step(pkt, ETH_P_IP);
		break;

	case ETH_P_IPV6:
		pkt->ipv6_header = get_ipv6hdr(buffer, &tmp_len);
		set_pkt_step(pkt, ETH_P_IPV6);
		break;

	default:
		break;
	}

	info("%p %p %u\n", buffer, *buffer, tmp_len);
	pkt->payload = *buffer;
	pkt->payload_len = tmp_len;

	return (0);
}
